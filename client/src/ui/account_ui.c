// Krunker v3.1.1 account in the main menu, over the brain's social socket
// (client/include/backend.h). Readable source: public/js/src/index.js
// (sendAccountRequest, accountResponse, updateAccount, cK, registerAcc, loginAcc,
// logoutAcc, readyLoad's token restore, windows[4] = the Account window) and
// src/data/account.js (Account.setData / updateLevel).
//
// v3.1.1 sends "a" on the game socket and the game server proxies it; KrunkNative's
// menu can sit on an offline game or a direct --connect server, so it authenticates
// on the brain's social socket directly (same frames: ["a", action, payload] ->
// ["a", error, id, username, data, token]). The game server learns the account from
// the session token / seek ticket in NET_PACKET_JOIN (client/src/net/net.c).
//
// The session is not kept in the game's storage: a listed account's session lives in the alt vault
// (client/src/alt/, the Account Manager). The v3.1.1 keys (krunker_id, krunker_username, krunker_token)
// are only read once, to move an older build's session into the list (load_session).
#include <v10_menu.h> // the v10 page's header bar (v10_menu.c)
#include "klog.h"
#include <player_names.h> // Hide player names (player_names.h)
#include <backend.h>
#include <backend_ui.h>
#include <krunker_link.h> // krunker_net_take_drop_instance: the drops the game server gave (account_add_skin)
#include <kn_captcha.h> // the sign-in's and the registration's captcha (doc/v10/captcha.md)
#include <alt_store.h> // the encrypted session vault (client/src/alt/)
#include <alt_manager_ui.h> // the Account Manager's answers to its Add / switch requests
#include <account_split.h>   // the account split: a krunker.io login is never sent to kn_brain (and the reverse)
#include <app_mode.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <ui_logic.h>
#include <v10_wild_offer.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#if defined(_WIN32)
#include <process.h>
#elif !defined(__EMSCRIPTEN__)
#include <unistd.h>
#endif
#include <stdint.h> // intptr_t: the email preference's callback argument
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define timegm _mkgmtime // MinGW has no timegm (same as editor_net.c)
#include <direct.h>      // _mkdir: the challenge capture's directory
#else
#include <sys/stat.h>    // mkdir: the challenge capture's directory
#endif
#include "account_flags.h"
#include <progression.h> // WP-E part 2c: v7 level progress, challenge names
#include <junk.h>        // the Junk wallet's amounts (doc/v10/junk.md)
#include <badges.h>      // WP-E part 2c: badges.js
#include <kid_settings.h> // Privacy & Age (v10.0.4): the k-ID section of Account Settings
#include <map_catalog.h> // WP-E part 2c: the Map Challenges cards
#include <weapons.h>
#include "../viewer/viewer_store.h" // WP-E part 2c: challenge reward skins (store.skins)
#include "qr_code.h"                // v7.1.4 2FA: the otpauth:// QR code (QrCode.toDataURL)
#include <ruleset.h>
#include <avatar_ui.h>      // profile pictures: the "Change Picture" picker and its status line
#include <v10_social.h> // the window's Friends tab (v10_social.c)
#include <online_mode.h>  // auth_unreachable: offline mode
#include <krunker_account.h> // the krunker.io account: the active one in an official game
#include <krunker_net.h>     // krunker_net_active(): an official game is being joined or played
#include <kn_ad.h>           // the placeholder ad: the Free KR card's (claimReward) (doc/v10/ads.md)
#include <video_ad.h>        // FORCE_VIDEO_AD on an official game: the placeholder video ad
#include <double_xp_offer.h> // the end screen's 2x XP answer on the krunker.io game socket ("rrqdxp")
#include <app_backend.h>     // app_backend_official_session(): the router's / a child window's official connection

#define RANK_VAR 0.03            // config.rankVar
#define ACC_STR_WAIT I18N("generic.wait", "Please wait...") // v7 showAccountResponse(i18n.t("generic.wait"))
#define REWARD_MIN_LVL 1             // the Free KR wheel's minLvl (v3.1.1's daily reward: config.rewardMinLvl 5)
#define REWARD_TIME_MS 3600000.0     // config.rewardTime 0x36ee80 (1 h, deob-10.0.5.js:19957; 12 h in v7.1.4)

static Client *g_client;
static AccountInfo g_acc;
static int g_logged_in;
static int g_header_dirty = 1;   // cK() pending (page not up yet / offsetWidth not laid out)
static int g_restore_sent;       // a token restore is in flight
// The saved session's asks (try_restore). The brain keeps a session's token as it is (auth.c: sliding expiry),
// so a refused one is asked once more, after RESTORE_RETRY_S, before the saved session is forgotten (refresh
// and retry); an ask with no answer after RESTORE_ANSWER_S is sent again. RESTORE_ASKS bounds both, per socket.
#define RESTORE_ASKS 2
#define RESTORE_RETRY_S 2.0
#define RESTORE_ANSWER_S 10.0
static int g_restore_asks;        // asks sent on this socket
static double g_restore_at;       // glfwGetTime() of the last ask (0 none)
static double g_restore_retry_at; // a refused ask is asked again at this glfwGetTime() (0 none)
// windows[4].tabIndex over profileTabs (deob-10.0.3.js:21519): home, Friends, Settings, Connections. v7.1.4's
// Partner and promo code (vpn_key) tabs are gone from 10.0.3 (the Partner program with them: window 28's gen is empty).
enum { PROFILE_TAB_HOME, PROFILE_TAB_FRIENDS, PROFILE_TAB_SETTINGS, PROFILE_TAB_CONNECTIONS, PROFILE_TAB_COUNT };
// The stat tabs: statTabs (deob-10.0.3.js:21520: General Stats, Killstreaks, Other Stats, Class Stats, Raids), then
// KrunkNative's Medals (kn_brain "mdl", a Box Office account only; doc/v10/kn-additions.md)
enum { STAT_TAB_RAIDS = 4, STAT_TAB_MEDALS = 5, STAT_TAB_COUNT = 6 };
static int g_tab_index;          // windows[4].tabIndex
static int g_promo_emails = -1;  // the email preference v10 caches (iiíììîî.promotional): -1 none stored (null)
static cJSON *g_raid_stats;      // the Box Office account's raidStats (owned; kn_brain's account info, raids.c)
static int g_req_del;            // acc.reqDel (v10 player_cancelrequest): "Processing Deletion" (account_life.c)
static char g_twitch_id[64];     // acc.twitchId (setData [44]): the linked Twitch user id, "" none
static char g_twitch_name[64];   // acc.twitch (setData [23]): the linked Twitch login, "" none
static int g_logout_sessions;    // d1: "Logout of all Sessions" clicked
static char g_token[192];
static char g_session_user[ALT_LABEL_MAX]; // the login name g_token belongs to ("" = no session)
static char g_guest_name[32];    // client->menu_name before the account took it over
static int g_reward_played;      // dc: a match ended this session (canClaimReward)
static char g_reward_text[16];   // d8: last #claimTimer text
// windows[29] (Krunker Mail)
static int g_mail_tab;           // the kml request / answer: 0 unread, 1 archive
static int g_mail_view;          // windows[29].tabIndex (v7: Followers, Clan, Hub, Inbox, Archive)
static cJSON *g_mail_rows;       // mailData.d (owned), NULL = not loaded
static int g_mail_loaded;        // mailData != null
static int g_mail_failed;        // the kml answer was refused, or none came by the deadline: the list says so
static int g_mail_timer;         // the deadline of the kml request in flight (html_timer id, 0 none)
// v7.1.4 2FA (authenticator app): v10808, the label sub2FA sends the code with ("2fa" = the
// login step, "a2FAc" / "d2FAc" = confirm enabling / disabling)
#define TWOFA_KEY_LEN 6 // config.twoFAKeyLen
static char g_2fa_label[8];
// KrunkNative email verification (kn_brain "emailS" / "cemail" / "emailv"; v7.1.4 has none)
static struct {
    int known, verified, pending, wait, mail;
    char addr[256];
} g_email;
static double g_auth_sent_at;    // a login/register is waiting for its "a" reply (glfwGetTime), 0 none
static unsigned g_auth_captcha;  // its captcha in flight (kn_captcha.h), 0 none: the answer's timeout waits for the send
// player-viewer WP-E part 2c: v7 challenges (windows[32]) and the account's medal counts
static cJSON *g_chal;            // account.activeChallenges from kn_brain's "chgL" (owned), NULL = not loaded
static int g_chal_requested;     // a "chg" request is out
static char g_chal_typ[12];      // windows[32].openChallenge.typ ("" = home)
static int g_chal_ind;           // windows[32].openChallenge.ind
static cJSON *g_medal_counts;    // "mdl" {medal: count} (owned), NULL = not loaded
static int g_scenario;           // account_ui_scenario: a stand-in account (no kn_brain)
static cJSON *g_scenario_c2fa;   // account.c2fa: a "c2fa" frame replayed after the window (the 2FA popup)
#define AUTH_TIMEOUT_SECONDS 8.0

// ------------------------------------------------------------------ helpers

static void storage_get(const char *key, char *out, const size_t n) {
    out[0] = 0;
    if (html_storage_get(key, out, n) < 0) out[0] = 0;
}

static void set_display(const char *id, const char *value) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void copy_str(char *dst, const size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

static double num_at(const cJSON *arr, const int i) {
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : 0.0;
}

static const char *str_at(const cJSON *arr, const int i) {
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// UTILS.commaNumber
static void comma_number(char *out, const size_t n, const double value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value);
    const char *d = digits;
    size_t o = 0;
    if (*d == '-') {
        if (o + 1 < n) out[o++] = '-';
        d++;
    }
    const size_t len = strlen(d);
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

// UTILS.getReadableTime(ms)
static void readable_time(char *out, const size_t n, const double ms) {
    long long s = (long long) floor(ms / 1000.0);
    long long m = s / 60;
    long long h = m / 60;
    m %= 60;
    const long long d = h / 24;
    h %= 24;
    char dp[24] = "", hp[24] = "";
    if (d) snprintf(dp, sizeof(dp), "%lldd ", d);
    if (h) snprintf(hp, sizeof(hp), "%lldh ", h);
    snprintf(out, n, "%s%s%lldm ", dp, hp, m);
}

void account_readable_time(char *out, const size_t n, const double ms) { readable_time(out, n, ms); }

// ------------------------------------------------------------------ state

// ---- the active account (backend_ui.h): krunker.io in an official game, krunk.io elsewhere

static int g_scenario_official; // --render-scenario's state.official: an official game without a socket

static long process_id(void) {
#if defined(_WIN32)
    return (long) _getpid();
#elif defined(__EMSCRIPTEN__)
    return 0;
#else
    return (long) getpid();
#endif
}

// The backend follows the server the player is connected to (the official krunker.io game this window
// joins or plays, or the one a window of this client's tree plays: app_backend.h), never the sign-in state.
AccountBackend account_backend(void) {
    return krunker_net_active() || app_backend_official_session() || g_scenario_official ? ACCOUNT_BACKEND_KRUNKER
                                                                                          : ACCOUNT_BACKEND_KRUNKIO;
}

// The brain gate (backend.h backend_set_brain_gate): no brain request while krunker.io is the active backend.
static int brain_gated(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

void account_ui_scenario_official(const int on) { g_scenario_official = on != 0; }

const AccountInfo *krunkio_account(void) { return g_logged_in ? &g_acc : NULL; }

// The krunker.io account as v10's `acc` in an official game: what the official game socket says of it
// (v10:222148's handlers), never kn_brain. "a" ACCOUNT_RESPONSE [error, id, username, data] builds it
// from data's player_* fields (new Account(id, name, data), v10:112759); "ua" UPDATE_ACCOUNT [data]
// replaces them (Account.setData, v10:112835: v10's own slot order); "uf" UPDATE_FUNDS [kr, junk]
// (v10:208196). Before the game server names it (the menu, a seek), the name and level krunker.io's
// sign-in and /player/me gave. Its staff rights are krunker.io's (player_developer, player_role).
static struct {
    cJSON *obj;            // the player_* object (owned), NULL until "a"
    int id;
    char name[64];
    unsigned gen;          // bumped by every frame that changed it
    unsigned built_gen, built_kio_gen;
} g_kio;
static AccountInfo g_kio_acc;

static void kio_forget(void) {
    cJSON_Delete(g_kio.obj);
    g_kio.obj = NULL;
    g_kio.id = 0;
    g_kio.name[0] = 0;
    g_kio.gen++;
}

static double obj_num(const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_kio.obj, key);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : cJSON_IsString(v) ? atof(v->valuestring) : 0.0;
}

static const char *obj_str(const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_kio.obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void obj_set(const char *key, const cJSON *v) {
    if (!g_kio.obj || !v) return;
    cJSON *copy = cJSON_Duplicate(v, 1);
    if (!copy) return;
    if (cJSON_GetObjectItemCaseSensitive(g_kio.obj, key)) cJSON_ReplaceItemInObjectCaseSensitive(g_kio.obj, key, copy);
    else cJSON_AddItemToObject(g_kio.obj, key, copy);
}

// "1,4,9" (player_badge / player_titles) or an array -> a number array (owned)
static cJSON *number_list(const cJSON *v) {
    cJSON *out = cJSON_CreateArray();
    if (cJSON_IsArray(v)) {
        const cJSON *e;
        cJSON_ArrayForEach(e, v) if (cJSON_IsNumber(e) || cJSON_IsString(e)) cJSON_AddItemToArray(out, cJSON_CreateNumber(cJSON_IsNumber(e) ? e->valuedouble : atof(e->valuestring)));
    } else if (cJSON_IsString(v)) {
        for (const char *p = v->valuestring; *p;) {
            if (isdigit((unsigned char) *p)) cJSON_AddItemToArray(out, cJSON_CreateNumber(atof(p)));
            while (*p && *p != ',') p++;
            if (*p == ',') p++;
        }
    }
    return out;
}

static void kio_build(void) {
    cJSON_Delete(g_kio_acc.stats);
    cJSON_Delete(g_kio_acc.data);
    cJSON_Delete(g_kio_acc.badges);
    memset(&g_kio_acc, 0, sizeof(g_kio_acc));
    const char *name = g_kio.name[0] ? g_kio.name : krunker_account_name();
    copy_str(g_kio_acc.name, sizeof(g_kio_acc.name), name);
    copy_str(g_kio_acc.alias, sizeof(g_kio_acc.alias), name);
    if (!g_kio.obj) { // signed in, not in a game yet: what the sign-in and /player/me said
        g_kio_acc.level = krunker_account_level() > 0 ? krunker_account_level() : 1;
        g_kio_acc.score = krunker_account_score();
        g_kio_acc.funds = krunker_account_funds();
        g_kio_acc.level_prog = progression_level_prog(g_kio_acc.score, RANK_VAR);
        return;
    }
    AccountInfo *a = &g_kio_acc;
    a->id = g_kio.id;
    a->kills = obj_num("player_kills");
    a->deaths = obj_num("player_deaths");
    a->wins = obj_num("player_wins");
    a->games = obj_num("player_games_played");
    a->funds = obj_num("player_funds");
    a->score = obj_num("player_score");
    a->time_played = obj_num("player_timeplayed");
    a->featured = (int) obj_num("player_featured");
    a->creator = (int) obj_num("creator_badge");
    a->hack = (int) obj_num("player_hack");
    a->is_dev = (int) obj_num("player_developer");
    const int role = (int) obj_num("player_role");
    a->is_mod = role == 2; // updatePerms: isMod = role 2 (a Deputy, role 3, gets no moderator actions: genList :214291)
    a->following = (int) obj_num("player_following");
    a->followers = (int) obj_num("player_followed");
    a->job_rating = (int) obj_num("jobrating");
    a->job_rating_pos = (int) obj_num("jobratingpositive");
    a->premium = obj_num("player_premium") > 0;
    const char *twofa = obj_str("player_2FA");
    a->has_2fa = (twofa && twofa[0]) || obj_num("player_2FA") > 0;
    const char *clan = obj_str("player_clan");
    copy_str(a->clan, sizeof(a->clan), clan ? clan : "");
    const char *alias = obj_str("player_alias");
    if (a->premium && alias && alias[0]) copy_str(a->alias, sizeof(a->alias), alias);
    a->junk = obj_num("player_jnk");
    a->ranked_points = obj_num("ranked_points");
    a->spending_tier = (int) obj_num("spending_tier"); // spending_tier || 0 (deob-10.0.3.js:128959): the store's spender-only KR cards
    a->level = progression_level(a->score, RANK_VAR);
    a->level_prog = progression_level_prog(a->score, RANK_VAR);
    // stats: player_stats is JSON text in the object form, an object in setData's
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(g_kio.obj, "player_stats");
    a->stats = cJSON_IsString(st) ? cJSON_Parse(st->valuestring) : cJSON_IsObject(st) ? cJSON_Duplicate(st, 1) : NULL;
    if (!cJSON_IsObject(a->stats)) {
        cJSON_Delete(a->stats);
        a->stats = cJSON_CreateObject();
    }
    a->badges = number_list(cJSON_GetObjectItemCaseSensitive(g_kio.obj, "player_badge"));
    // the client's account data array (set_data's slots: kn_brain sends v7's order), so the
    // inventory (10), premium time (27) and the rest read the krunker.io account like the other
    a->data = cJSON_CreateArray();
    for (int i = 0; i < 40; i++) cJSON_AddItemToArray(a->data, cJSON_CreateNull());
    const struct {
        int slot;
        const char *key;
    } map[] = {{1, "player_kills"}, {2, "player_wins"}, {3, "player_games_played"}, {4, "player_deaths"}, {5, "player_funds"},
               {6, "player_score"}, {7, "player_clan"}, {8, "player_timeplayed"}, {9, "player_featured"}, {10, "skins"},
               {11, "player_lastreward"}, {12, "player_developer"}, {13, "player_following"}, {14, "player_followed"},
               {17, "player_hack"}, {27, "player_premium"}, {28, "player_2FA"}, {29, "player_alias"}, {38, "jobrating"}, {39, "jobratingpositive"}};
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(g_kio.obj, map[i].key);
        if (v) cJSON_ReplaceItemInArray(a->data, map[i].slot, cJSON_Duplicate(v, 1));
    }
    if (!cJSON_IsArray(cJSON_GetArrayItem(a->data, 10))) cJSON_ReplaceItemInArray(a->data, 10, cJSON_CreateArray());
    cJSON_ReplaceItemInArray(a->data, 16, cJSON_Duplicate(a->stats, 1));
    cJSON_ReplaceItemInArray(a->data, 21, cJSON_CreateNumber(a->is_mod));
}

static const AccountInfo *kio_account(void) {
    if (!krunker_account_signed_in()) {
        if (g_kio.obj) kio_forget();
        return NULL;
    }
    const unsigned kgen = krunker_account_generation();
    if (g_kio.built_gen != g_kio.gen || g_kio.built_kio_gen != kgen || (!g_kio_acc.name[0])) {
        g_kio.built_gen = g_kio.gen;
        g_kio.built_kio_gen = kgen;
        kio_build();
    }
    if (!g_kio.obj) { // the game server's "a" not seen (yet): its staff rights as krunker_net knows them
        KrunkerNetAccount k;
        const int in_game = krunker_net_account(&k);
        g_kio_acc.is_dev = in_game && k.dev;
        g_kio_acc.is_mod = in_game && k.role == 2;
    }
    return &g_kio_acc;
}

// v10's Account.setData (v10:112835): "ua" carries the account in v10's slot order
static void kio_set_data(const cJSON *d) {
    if (!cJSON_IsArray(d) || !g_kio.obj) return;
    const struct {
        int slot;
        const char *key;
    } map[] = {{1, "player_kills"}, {2, "player_wins"}, {3, "player_games_played"}, {4, "player_deaths"}, {5, "player_funds"},
               {6, "player_score"}, {7, "player_clan"}, {8, "player_timeplayed"}, {9, "player_featured"}, {10, "skins"},
               {11, "player_lastreward"}, {12, "player_developer"}, {13, "player_following"}, {14, "player_followed"},
               {15, "player_stats"}, {16, "player_hack"}, {17, "player_region"}, {18, "player_role"}, {19, "player_premium"},
               {20, "player_2FA"}, {21, "player_alias"}, {27, "jobrating"}, {28, "jobratingpositive"}, {30, "player_clanrole"},
               {37, "player_jnk"}, {38, "activeChallenges"}, {39, "player_badge"},
               {47, "ranked_points"}, {48, "spending_tier"}, // (rankedPoints: setData's [47], v10:112873; spendingTier [48], deob-10.0.3.js:129017)
               {52, "raidStats"}}; // raidStats: setData's [52] (deob-10.0.3.js:129021; the constructor's is [])
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        const cJSON *v = cJSON_GetArrayItem(d, map[i].slot);
        if (v) obj_set(map[i].key, v);
    }
}

// KRUNKNATIVE_CHALLENGE_CAPTURE: the krunker.io account's challenge catalogue, as it arrives, to a JSON file
// (doc/v10/challenges.md 4c). The catalogue is only ever delivered to a signed-in client (activeChallenges,
// account slot 38, with "a" / "ua"), so this is how Box Office gets the official set:
// tools/brain/challenges_from_capture.py turns the file into a brain migration. "1" writes
// $XDG_CACHE_HOME/krunknative/official-challenges-<date>.json (else ~/.cache/...), any other value is the
// file's path; unset / "" / "0" = off (the default; nothing is written). Shape: {"format": 1, "source",
// "game", "captured_at", "activeChallenges": [the latest full list], "frames": [{"at", "label", "args"}]}
// with every later "chg" / "uchp" / "chgC" / "chlR" frame (UPDATE_CHALLENGES[_PROGRESS], CHALLENGE_COMPLETED,
// UPDATE_CHALLENGE_REWARDS) appended. Rewritten whole at each event; no account id, name or token.
static cJSON *g_chal_capture; // the document (owned), NULL until the first captured frame
static const char *chal_capture_path(void) {
    static char path[1024];
    static int done;
    if (done) return path;
    done = 1;
    path[0] = 0;
    const char *knob = getenv("KRUNKNATIVE_CHALLENGE_CAPTURE");
    if (!knob || !*knob || !strcmp(knob, "0")) return path;
    if (strcmp(knob, "1") != 0) {
        snprintf(path, sizeof(path), "%s", knob);
        return path;
    }
#ifndef __EMSCRIPTEN__
    char dir[768];
    const char *xdg = getenv("XDG_CACHE_HOME");
    int n;
    if (xdg && *xdg) n = snprintf(dir, sizeof(dir), "%s/krunknative", xdg);
    else {
#ifdef _WIN32
        const char *base = getenv("LOCALAPPDATA");
        n = base && *base ? snprintf(dir, sizeof(dir), "%s\\krunknative", base) : 0;
#else
        const char *home = getenv("HOME");
        n = home && *home ? snprintf(dir, sizeof(dir), "%s/.cache/krunknative", home) : 0;
#endif
    }
    if (n <= 0 || (size_t) n >= sizeof(dir)) return path;
#ifdef _WIN32
    _mkdir(dir);
#else
    mkdir(dir, 0755);
#endif
    char day[16];
    const time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    strftime(day, sizeof(day), "%Y-%m-%d", &tmv);
    snprintf(path, sizeof(path), "%s/official-challenges-%s.json", dir, day);
#endif
    return path;
}

static void chal_capture(const char *label, const cJSON *frame) {
    const char *path = chal_capture_path();
    if (!path[0]) return;
    const int account = !strcmp(label, "a") || !strcmp(label, "ua");
    const int challenge = !strcmp(label, "chg") || !strcmp(label, "uchp") || !strcmp(label, "chgC") || !strcmp(label, "chlR");
    if (!account && !challenge) return;
    char stamp[32];
    const time_t now = time(NULL);
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &now);
#else
    gmtime_r(&now, &tmv);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    if (!g_chal_capture) {
        g_chal_capture = cJSON_CreateObject();
        cJSON_AddNumberToObject(g_chal_capture, "format", 1);
        cJSON_AddStringToObject(g_chal_capture, "source", "krunker.io");
        cJSON_AddStringToObject(g_chal_capture, "game", ruleset_current()->version);
        cJSON_AddStringToObject(g_chal_capture, "captured_at", stamp);
        cJSON_AddItemToObject(g_chal_capture, "activeChallenges", cJSON_CreateArray());
        cJSON_AddItemToObject(g_chal_capture, "frames", cJSON_CreateArray());
    }
    if (account) { // the list as the account carries it now (kio_set_data ran first)
        const cJSON *list = cJSON_GetObjectItemCaseSensitive(g_kio.obj, "activeChallenges");
        if (!cJSON_IsArray(list)) return; // (an account row without the list: nothing new)
        cJSON_ReplaceItemInObjectCaseSensitive(g_chal_capture, "activeChallenges", cJSON_Duplicate(list, 1));
    } else {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddStringToObject(f, "at", stamp);
        cJSON_AddStringToObject(f, "label", label);
        cJSON *args = cJSON_CreateArray();
        for (int i = 1; i < cJSON_GetArraySize(frame); i++) cJSON_AddItemToArray(args, cJSON_Duplicate(cJSON_GetArrayItem(frame, i), 1));
        cJSON_AddItemToObject(f, "args", args);
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(g_chal_capture, "frames"), f);
    }
    char *text = cJSON_Print(g_chal_capture);
    if (!text) return;
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    const int ok = fp && fwrite(text, 1, strlen(text), fp) == strlen(text) && fputc('\n', fp) != EOF && fclose(fp) == 0;
    if (fp && !ok) fclose(fp);
    if (ok && rename(tmp, path) == 0)
        KN_LOG_DEBUG1("[account] challenge capture: %s (%d challenges, %d frames)\n", path,
               cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(g_chal_capture, "activeChallenges")),
               cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(g_chal_capture, "frames")));
    else printf("[account] challenge capture: cannot write %s\n", path);
    free(text);
}

// The official game socket's account frames (krunker_net_account_listen), main thread
static void kio_frame(const char *label, const cJSON *frame) {
    if (strcmp(label, "a") && strcmp(label, "ua")) chal_capture(label, frame); // ("a" / "ua": after the account is set)
    if (!strcmp(label, "a")) {
        const cJSON *err = cJSON_GetArrayItem(frame, 1), *data = cJSON_GetArrayItem(frame, 4);
        const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 3));
        if ((cJSON_IsString(err) && err->valuestring[0]) || !name || !(cJSON_IsObject(data) || cJSON_IsArray(data))) return;
        cJSON_Delete(g_kio.obj);
        if (cJSON_IsArray(data)) { // v10: new Account(id, name, null), then setData(row) (v10:70182, v10:70129): the slot order
            g_kio.obj = cJSON_CreateObject();
            kio_set_data(data);
        } else {
            g_kio.obj = cJSON_Duplicate(data, 1);
        }
        const cJSON *id = cJSON_GetArrayItem(frame, 2);
        g_kio.id = cJSON_IsNumber(id) ? id->valueint : 0;
        copy_str(g_kio.name, sizeof(g_kio.name), name);
        g_kio.gen++;
        chal_capture("a", frame);
        menu_rerender_if_open(33); // the open Challenges window: its list is account.activeChallenges (v10_rewards.c)
        if (account_backend() == ACCOUNT_BACKEND_KRUNKER) battlepass_ui_account_answered(); // io_accountResponse: windows[5].data || send("bpdl")
        else KN_LOG_DEBUG1("[account] (the krunker.io account's data arrived while %s is the active backend)\n", "krunk.io");
        KN_LOG_DEBUG1("[account] the krunker.io account's data from the game server: %s, %.0f KR, %d skins\n", name, obj_num("player_funds"),
               cJSON_GetArraySize(cJSON_GetObjectItemCaseSensitive(g_kio.obj, "skins")));
    } else if (!strcmp(label, "ua")) {
        kio_set_data(cJSON_GetArrayItem(frame, 1));
        g_kio.gen++;
        chal_capture("ua", frame);
        menu_rerender_if_open(33); // the open Challenges window (activeChallenges arrives in "ua" slot 38)
    } else if (!strcmp(label, "uf")) { // [kr, junk]
        const cJSON *kr = cJSON_GetArrayItem(frame, 1), *jnk = cJSON_GetArrayItem(frame, 2);
        if (cJSON_IsNumber(kr)) obj_set("player_funds", kr);
        if (cJSON_IsNumber(jnk)) obj_set("player_jnk", jnk);
        g_kio.gen++;
    } else if (!strcmp(label, "ub")) { // UPDATE_BALANCES [balances]: rankedPoints is balances[3].amount (v10:208200)
        const cJSON *rp = cJSON_GetArrayItem(cJSON_GetArrayItem(frame, 1), 3);
        const cJSON *amount = cJSON_GetObjectItemCaseSensitive(rp, "amount");
        if (cJSON_IsNumber(amount)) obj_set("ranked_points", amount);
        g_kio.gen++;
    } else if (!strcmp(label, "chlR")) { // UPDATE_CHALLENGE_REWARDS: arg 6 = the points won, arg 7 the balance (v10:208212-208248)
        const cJSON *won = cJSON_GetArrayItem(frame, 7), *balance = cJSON_GetArrayItem(frame, 8);
        if (won && cJSON_IsNumber(won) && won->valuedouble != 0 && cJSON_IsNumber(balance)) obj_set("ranked_points", balance);
        g_kio.gen++;
    } else if (account_backend() != ACCOUNT_BACKEND_KRUNKER) {
        return; // (the battle pass / clan / challenge answers belong to the krunker.io account's windows)
    } else if (!strcmp(label, "bpdl") || !strcmp(label, "bppd") || !strcmp(label, "bpEnd") || !strcmp(label, "bpclmr")) {
        // the pass, its progress, the end's tmpBPData ("end" [6], krunker_link.c), a card's claim (claimItemRes, :217560)
        battlepass_ui_frame(label, frame);
    } else if (!strcmp(label, "spinKR")) { // SPIN_KR: the Free KR wheel's prize (iïiiîïì: lastReward, rand, index, funds, err)
        const cJSON *last = cJSON_GetArrayItem(frame, 1), *funds = cJSON_GetArrayItem(frame, 4);
        if (cJSON_IsNumber(last)) obj_set("player_lastreward", last);
        if (cJSON_IsNumber(funds)) account_kio_set_balances(1, funds->valuedouble, 0, 0);
        store_ui_frame(frame);
    } else if (!strcmp(label, "rrqdxp")) { // RECIEVE_DOUBLE_XP: the end screen's 2x XP (deob-10.0.5.js:227958)
        double_xp_offer_answer(frame);
    } else if (!strcmp(label, "vad")) { // FORCE_VIDEO_AD: forceVideoAd (:227964-227981), the placeholder ad
        client_video_ad_force(); // its CUSTOM_VIDEO_AD states back on the game socket (video_ad.c)
    } else if (!strcmp(label, "cln")) {
        clan_ui_frame(frame);
    } else if (!strcmp(label, "dslr")) { // DAILY_SPIN_CLAIM_RESPONSE [{ticketId}] (v10_daily_spin.c)
        v10_daily_spin_frame(label, frame);
    } else if (!strcmp(label, "_0") || !strcmp(label, "_1")) { // an rpc's answer, named (krunker_net.h): the claim's
        const char *rpc = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
        if (rpc && !strcmp(rpc, "dsl")) v10_daily_spin_frame(label, frame);
    } else if ((!strcmp(label, "chg") || !strcmp(label, "uchp")) && g_kio.obj) {
        // UPDATE_CHALLENGES [PT, CI, PV]... / UPDATE_CHALLENGES_PROGRESS [CI, PV]... (v10:218110 / 218121)
        cJSON *list = cJSON_GetObjectItemCaseSensitive(g_kio.obj, "activeChallenges");
        const cJSON *d = cJSON_GetArrayItem(frame, 1);
        const int step = !strcmp(label, "chg") ? 3 : 2, n = cJSON_GetArraySize(d);
        for (int i = 0; cJSON_IsArray(list) && i + step - 1 < n; i += step) {
            const double ci = cJSON_GetNumberValue(cJSON_GetArrayItem(d, i + (step == 3 ? 1 : 0)));
            cJSON *c;
            cJSON_ArrayForEach(c, list) {
                const cJSON *cci = cJSON_GetObjectItemCaseSensitive(c, "CI");
                if (!cJSON_IsNumber(cci) || cci->valuedouble != ci) continue;
                const cJSON *pv = cJSON_GetArrayItem(d, i + (step == 3 ? 2 : 1));
                if (pv) cJSON_ReplaceItemInObjectCaseSensitive(c, "PV", cJSON_Duplicate(pv, 1));
                if (step == 3) cJSON_ReplaceItemInObjectCaseSensitive(c, "PT", cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetArrayItem(d, i))));
            }
        }
        menu_rerender_if_open(33);
    }
}

// The windows' frames while krunker.io is the active backend (backend_social_set_held_sender): the
// read-only ones go over the official game socket, as v10 sends them (krunker_net.h)
static int kio_held_send(const char *json) {
    if (account_backend() != ACCOUNT_BACKEND_KRUNKER) return 0;
    cJSON *frame = cJSON_Parse(json);
    const int sent = frame ? krunker_net_send_account_frame(frame) : 0;
    cJSON_Delete(frame);
    return sent;
}

const AccountInfo *active_account(void) {
    return account_backend() == ACCOUNT_BACKEND_KRUNKER ? kio_account() : krunkio_account();
}

// What the UI last followed: the backend, the krunk.io login and the krunker.io account's state
static struct {
    int backend, kn_logged_in, kio_signed_in;
    unsigned kio_gen, kio_data_gen, generation;
} g_active = {.backend = -1};

unsigned account_active_generation(void) { return g_active.generation; }

// One no-argument social frame (v10 "ttl": the account's titles; v10_customize.c). The frame
// goes through the same queue as the mail frames (backend_social_send_text).
void account_send_frame(const char *label) {
    char frame[64];
    snprintf(frame, sizeof(frame), "[\"%s\"]", label);
    backend_social_send_text(frame);
}

static void mark_header(void);
static void rerender_windows(void);
static void active_tick(void);
static void update_header(void);

static void reward_tick(int force);

void account_remove_skin(const int skin_index) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return; // (kn_brain's answers: the krunk.io account's, never the krunker.io one)
    if (!g_logged_in || !g_acc.data) return;
    cJSON *skins = cJSON_GetArrayItem(g_acc.data, 10);
    cJSON *entry;
    int i = 0;
    cJSON_ArrayForEach(entry, skins) {
        cJSON *ind = cJSON_GetObjectItem(entry, "ind"), *cnt = cJSON_GetObjectItem(entry, "cnt");
        if (cJSON_IsNumber(ind) && ind->valueint == skin_index) {
            const int n = (cJSON_IsNumber(cnt) ? cnt->valueint : 1) - 1;
            if (n > 0) cJSON_ReplaceItemInObject(entry, "cnt", cJSON_CreateNumber(n));
            else cJSON_DeleteItemFromArray(skins, i);
            return;
        }
        i++;
    }
}

// One unit of a stack entry (no id) taken; the entry goes at 0.
static void take_stack_unit(cJSON *skins, cJSON *entry) {
    const cJSON *cnt = cJSON_GetObjectItem(entry, "cnt");
    const int n = (cJSON_IsNumber(cnt) ? cnt->valueint : 1) - 1;
    if (n > 0) {
        cJSON_ReplaceItemInObject(entry, "cnt", cJSON_CreateNumber(n));
        return;
    }
    int i = 0;
    cJSON *x;
    cJSON_ArrayForEach(x, skins) {
        if (x == entry) break;
        i++;
    }
    cJSON_DeleteItemFromArray(skins, i);
}

// A paint applied (POST /items/<sid>/paint, client/src/ui/paint_picker.c): one unit of the paint is taken; the copy
// copy_id gets the paint m (a new copy, or a repaint). from_stack: sid was an unpainted unit's stack, so one of its units
// became the copy. The brain pushes the account again ("ua") after this, which replaces it.
void account_paint_applied(const int item, const int paint, const int copy_id, const int from_stack, const cJSON *m) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return;
    if (!g_logged_in || !g_acc.data) return;
    cJSON *skins = cJSON_GetArrayItem(g_acc.data, 10);
    if (!cJSON_IsArray(skins)) return;
    cJSON *e, *copy = NULL, *paint_stack = NULL, *item_stack = NULL;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItem(e, "ind"), *id = cJSON_GetObjectItem(e, "id");
        if (!cJSON_IsNumber(ind)) continue;
        if (cJSON_IsNumber(id)) {
            if (id->valueint == copy_id) copy = e;
        } else if (ind->valueint == paint && !paint_stack) {
            paint_stack = e;
        } else if (ind->valueint == item && !item_stack) {
            item_stack = e;
        }
    }
    if (copy) {
        cJSON_ReplaceItemInObject(copy, "m", m ? cJSON_Duplicate(m, 1) : cJSON_CreateObject());
    } else {
        cJSON *fresh = cJSON_CreateObject();
        cJSON_AddNumberToObject(fresh, "ind", item);
        cJSON_AddNumberToObject(fresh, "cnt", 1);
        cJSON_AddNumberToObject(fresh, "id", copy_id);
        cJSON_AddItemToObject(fresh, "m", m ? cJSON_Duplicate(m, 1) : cJSON_CreateObject());
        cJSON_AddNumberToObject(fresh, "st", 0);
        cJSON_AddItemToArray(skins, fresh);
        if (from_stack && item_stack) take_stack_unit(skins, item_stack);
    }
    if (paint_stack) take_stack_unit(skins, paint_stack);
}

void account_set_last_reward(const double ms) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return; // (kn_brain's answers: the krunk.io account's, never the krunker.io one)
    if (!g_logged_in || !g_acc.data) return;
    cJSON_ReplaceItemInArray(g_acc.data, 11, cJSON_CreateNumber(ms));
    reward_tick(1);
}

void account_set_funds(const double funds) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return; // (kn_brain's answers: the krunk.io account's, never the krunker.io one)
    if (!g_logged_in) return;
    g_acc.funds = funds;
    if (g_acc.data) cJSON_ReplaceItemInArray(g_acc.data, 5, cJSON_CreateNumber(funds));
    mark_header();
}

void account_kio_set_balances(const int has_funds, const double funds, const int has_junk, const double junk) {
    if (account_backend() != ACCOUNT_BACKEND_KRUNKER || !g_kio.obj) return;
    cJSON *v = NULL;
    if (has_funds && (v = cJSON_CreateNumber(funds))) obj_set("player_funds", v);
    cJSON_Delete(v);
    v = NULL;
    if (has_junk && (v = cJSON_CreateNumber(junk))) obj_set("player_jnk", v);
    cJSON_Delete(v);
    g_kio.gen++;
}

long account_skin_stat(const int skin, const char *stat) {
    if (account_backend() != ACCOUNT_BACKEND_KRUNKER || !g_kio.obj || !stat || skin < 0) return -1;
    const cJSON *skins = cJSON_GetObjectItemCaseSensitive(g_kio.obj, "skins"), *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind");
        if (!cJSON_IsNumber(ind) || ind->valueint != skin) continue;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "m");
        cJSON *parsed = cJSON_IsString(m) && m->valuestring ? cJSON_Parse(m->valuestring) : NULL; // a JSON-string m
        if (parsed) m = parsed;
        const cJSON *v = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, stat) : NULL;
        const long n = cJSON_IsNumber(v) && v->valuedouble >= 0 ? (long) v->valuedouble : -1;
        cJSON_Delete(parsed);
        if (n >= 0) return n; // the first entry of that index with a count (the plain-index rule, as the paint)
    }
    return -1;
}

int account_skin_seed(const int skin) {
    if (account_backend() != ACCOUNT_BACKEND_KRUNKER || !g_kio.obj || skin < 0) return -1;
    const cJSON *skins = cJSON_GetObjectItemCaseSensitive(g_kio.obj, "skins"), *e;
    int found = 0;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind");
        if (!cJSON_IsNumber(ind) || ind->valueint != skin) continue;
        found = 1;
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(e, "m"), *id = cJSON_GetObjectItemCaseSensitive(e, "id");
        cJSON *parsed = cJSON_IsString(m) && m->valuestring ? cJSON_Parse(m->valuestring) : NULL;
        if (parsed) m = parsed;
        const cJSON *seed = cJSON_IsObject(m) ? cJSON_GetObjectItemCaseSensitive(m, "seed") : NULL;
        const int32_t s = skin_copy_seed(cJSON_IsNumber(id) ? (int64_t) id->valuedouble : 0, cJSON_IsNumber(seed),
                                         cJSON_IsNumber(seed) ? seed->valuedouble : 0.0);
        cJSON_Delete(parsed);
        if (s) return s; // the first copy of that index with a seed (the plain-index rule, as the paint)
    }
    return found ? 0 : -1;
}

int account_add_skin(const int skin_index) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) {
        // the krunker.io account's own (the game socket's "dslr"): v10's account.addSkin(ind) (deob-10.0.3.js
        // 129095-129120): an owned, unlisted entry's cnt += 1, else push {ind, cnt: 1, st: 1}
        if (!g_kio.obj || !krunker_account_signed_in()) return 0;
        cJSON *skins = cJSON_GetObjectItemCaseSensitive(g_kio.obj, "skins");
        if (!cJSON_IsArray(skins)) {
            cJSON_DeleteItemFromObjectCaseSensitive(g_kio.obj, "skins");
            skins = cJSON_AddArrayToObject(g_kio.obj, "skins");
            if (!skins) return 0;
        }
        cJSON *entry;
        int iid = 0;
        cJSON *meta = NULL;
        if (krunker_net_take_drop_instance(skin_index, &iid, &meta)) {
            // v10.0.4's addSkin(ind, 1, iid, meta): the instance is its own entry (deob-10.0.4.js:115078-115087), so a
            // drop is equipped by its id without a relog (the SKIN_REWARD handler, deob-10.0.4.js:221926)
            entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "ind", skin_index);
            cJSON_AddNumberToObject(entry, "cnt", 1);
            cJSON_AddNumberToObject(entry, "id", iid);
            cJSON_AddItemToObject(entry, "m", meta ? meta : cJSON_CreateObject());
            cJSON_AddNumberToObject(entry, "st", 0);
            cJSON_AddItemToArray(skins, entry);
            g_kio.gen++;
            return 1;
        }
        int n = 0;
        cJSON_ArrayForEach(entry, skins) {
            const cJSON *ind = cJSON_GetObjectItemCaseSensitive(entry, "ind");
            if (!cJSON_IsNumber(ind) || ind->valueint != skin_index || cJSON_GetObjectItemCaseSensitive(entry, "id")) continue;
            const cJSON *cnt = cJSON_GetObjectItemCaseSensitive(entry, "cnt");
            n = (cJSON_IsNumber(cnt) ? cnt->valueint : 0) + 1;
            cJSON_DeleteItemFromObjectCaseSensitive(entry, "cnt");
            cJSON_AddNumberToObject(entry, "cnt", n);
            break;
        }
        if (!n) {
            entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "ind", skin_index);
            cJSON_AddNumberToObject(entry, "cnt", 1);
            cJSON_AddNumberToObject(entry, "st", 1);
            cJSON_AddItemToArray(skins, entry);
            n = 1;
        }
        g_kio.gen++;
        return n;
    }
    if (!g_logged_in || !g_acc.data) return 0;
    cJSON *skins = cJSON_GetArrayItem(g_acc.data, 10);
    if (!cJSON_IsArray(skins)) {
        skins = cJSON_CreateArray();
        cJSON_ReplaceItemInArray(g_acc.data, 10, skins);
    }
    cJSON *entry;
    cJSON_ArrayForEach(entry, skins) {
        cJSON *ind = cJSON_GetObjectItem(entry, "ind");
        if (cJSON_IsNumber(ind) && ind->valueint == skin_index) {
            cJSON *cnt = cJSON_GetObjectItem(entry, "cnt");
            const int n = (cJSON_IsNumber(cnt) ? cnt->valueint : 0) + 1;
            cJSON_ReplaceItemInObject(entry, "cnt", cJSON_CreateNumber(n));
            return n;
        }
    }
    entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "ind", skin_index);
    cJSON_AddNumberToObject(entry, "cnt", 1);
    cJSON_AddItemToArray(skins, entry);
    return 1;
}
const char *account_session_token(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKIO ? g_token : ""; }

// windows[12]: acc.clan = clanData.clan_name / null (data slot 7 follows)
void account_set_clan(const char *tag) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return; // (kn_brain's answers: the krunk.io account's, never the krunker.io one)
    if (!g_logged_in) return;
    copy_str(g_acc.clan, sizeof(g_acc.clan), tag ? tag : "");
    if (g_acc.data) cJSON_ReplaceItemInArray(g_acc.data, 7, tag && tag[0] ? cJSON_CreateString(tag) : cJSON_CreateNull());
    mark_header();
}

// Account.updateLevel(): levelProg is v7.1.4's getLevelProg (linear in score, WP-E part 2c)
static void update_level(void) {
    g_acc.level = progression_level(g_acc.score, RANK_VAR);
    g_acc.level_prog = progression_level_prog(g_acc.score, RANK_VAR);
}

// Account.setData(h)
static void set_data(const cJSON *h) {
    if (!cJSON_IsArray(h)) return;
    g_acc.kills = num_at(h, 1);
    g_acc.wins = num_at(h, 2);
    g_acc.games = num_at(h, 3);
    g_acc.deaths = num_at(h, 4);
    g_acc.funds = num_at(h, 5);
    g_acc.score = num_at(h, 6);
    update_level();
    copy_str(g_acc.clan, sizeof(g_acc.clan), str_at(h, 7));
    g_acc.time_played = num_at(h, 8);
    g_acc.featured = (int) num_at(h, 9);
    g_acc.is_dev = (int) num_at(h, 12);
    g_acc.following = (int) num_at(h, 13);
    g_acc.followers = (int) num_at(h, 14);
    cJSON_Delete(g_acc.stats);
    const cJSON *stats = cJSON_GetArrayItem(h, 16);
    g_acc.stats = cJSON_IsObject(stats) ? cJSON_Duplicate(stats, 1) : cJSON_CreateObject();
    g_acc.hack = (int) num_at(h, 17);
    g_acc.is_mod = (int) num_at(h, 21);
    g_acc.elo = num_at(h, 22); // player_elo: this season's ranked rating (kn_brain ranked.c)
    g_acc.premium = num_at(h, 27) > 0;
    g_acc.job_rating = (int) num_at(h, 38);     // jobRating: the K.P.D calls an officer rated
    g_acc.job_rating_pos = (int) num_at(h, 39); // jobRatingPos: the ones rated tagged
    g_acc.has_2fa = (int) num_at(h, 28);
    const char *alias = str_at(h, 29);
    copy_str(g_acc.alias, sizeof(g_acc.alias), g_acc.premium && alias && alias[0] ? alias : g_acc.name);
    cJSON_Delete(g_acc.data);
    g_acc.data = cJSON_Duplicate(h, 1);
}

static double stat_num(const char *key) {
    const cJSON *v = g_acc.stats ? cJSON_GetObjectItem(g_acc.stats, key) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

// The account name is the name the menu and HUD show (v3.1.1: acc.alias / "Guest").
// The active account's (backend_ui.h): in an official game the krunker.io account's name or the
// guest's own, never the krunk.io account's.
static void apply_menu_name(void) {
    if (!g_client) return;
    const AccountInfo *acc = active_account();
    if (acc) {
        if (!g_guest_name[0]) copy_str(g_guest_name, sizeof(g_guest_name), g_client->menu_name);
        copy_str(g_client->menu_name, sizeof(g_client->menu_name), acc->alias);
    } else if (g_guest_name[0]) {
        copy_str(g_client->menu_name, sizeof(g_client->menu_name), g_guest_name);
        g_guest_name[0] = 0;
    }
    // the game server reads the session token from the JOIN packet of the next connection
    pthread_mutex_lock(&g_client->net_lock);
    copy_str(g_client->net_join.session, sizeof(g_client->net_join.session), g_logged_in ? account_session_token() : ""); // (a krunker.io game gets no Box Office token)
    pthread_mutex_unlock(&g_client->net_lock);
}

// ------------------------------------------------------------------ cK(): header bar

// updateMenuAccountData(): Krunker v10's header bar and nav (v10_menu.c). Not the main page yet
// (loading, or the hub / editor page): again on a later tick.
static void update_header(void) {
    g_header_dirty = 0;
    if (!v10_menu_page()) {
        g_header_dirty = 1;
        return;
    }
    v10_menu_account_changed();
}

static void mark_header(void) {
    g_header_dirty = 1;
}

// showAccResp(html)
static void show_acc_resp(const char *html) {
    HtmlElem el = html_dom_by_id("accResp");
    if (!el) return;
    html_dom_set_html(el, html ? html : "");
    html_dom_set_display(el, html && html[0] ? "block" : "none");
}

// popupResponse(kind, error, text) / dP(text) over the menu's #popupHolder > #popupContent
static void popup_response(const int kind, const char *error, const char *text) {
    static const char *const pops[] = {"popupContent", "confPremiumPop", "gameCodePop", "tradePop", "twitchPop", "pfpPop", "dropsPop", "confPop"};
    set_display("popupHolder", "block");
    for (size_t i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) set_display(pops[i], "none");
    HtmlElem content = html_dom_by_id("popupContent");
    if (!content) return;
    html_dom_set_style(content, "width", "300px");
    html_dom_set_display(content, "block");
    StrBuf sb = {0};
    if (kind == 2) {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div class='lds-ring sm'><div></div><div></div><div></div><div></div></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append_esc(&sb, text ? text : "");
    } else if (kind == 1) {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#3be354'>done</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append_esc(&sb, text && text[0] ? text : "Success!");
    } else {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#e33b3b'>clear</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append_esc(&sb, error && error[0] ? error : "Failed. Try Again");
    }
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    html_dom_set_html(content, html);
    free(html);
}

void account_popup(const int kind, const char *error, const char *text) { popup_response(kind, error, text); }

static void rerender_windows(void) {
    menu_rerender_if_open(5);
    menu_rerender_if_open(6); // v7.1.4 Battle Pass (battlepass_ui.c)
    menu_rerender_if_open(14);
    menu_rerender_if_open(27);
    menu_rerender_if_open(33);
    menu_rerender_if_open(13);
}

// ------------------------------------------------------------------ requests

// A sign-in's frame, once its captcha is done: the answer's timeout (AUTH_TIMEOUT_SECONDS) counts from here
static void send_auth_frame(const cJSON *frame) {
    g_auth_captcha = 0;
    backend_social_send(frame);
    if (g_auth_sent_at > 0) g_auth_sent_at = glfwGetTime();
}

// sendAccountRequest(action, payload); email: KrunkNative's optional 4th payload element (register)
static void send_account_request_email(const int action, const char *name, const char *pass, const char *third,
                                       const char *email) {
    if (action >= 0) show_acc_resp(ACC_STR_WAIT);
    else {
        kn_captcha_cancel(g_auth_captcha); // a logout drops a sign-in still solving its captcha
        g_auth_captcha = 0;
    }
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("a"));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(action));
    if (action >= 0) {
        cJSON *payload = cJSON_CreateArray();
        cJSON_AddItemToArray(payload, cJSON_CreateString(name ? name : ""));
        cJSON_AddItemToArray(payload, pass ? cJSON_CreateString(pass) : cJSON_CreateNull());
        cJSON_AddItemToArray(payload, third ? cJSON_CreateString(third) : cJSON_CreateNull());
        if (email && email[0]) cJSON_AddItemToArray(payload, cJSON_CreateString(email));
        cJSON_AddItemToArray(frame, payload);
        if (action == 0 || pass) {
            // the captcha key: Box Office's ALTCHA payload for a password sign-in or a registration (kn_captcha.h; v10.0.3
            // asks Turnstile before every loginUsername, deob-10.0.3.js:130762). A second click sends only the last one
            kn_captcha_cancel(g_auth_captcha);
            g_auth_captcha = kn_captcha_send_frame(action == 0 ? "register" : "login", frame, 3, send_auth_frame);
            return;
        }
        cJSON_AddItemToArray(frame, cJSON_CreateNull()); // captcha key: none for a saved session's resume
    }
    backend_social_send(frame);
    cJSON_Delete(frame);
}

static void send_account_request(const int action, const char *name, const char *pass, const char *third) {
    send_account_request_email(action, name, pass, third, NULL);
}

// The session is kept in memory (g_token, g_session_user) and stored only for an account the Account
// Manager lists (the sealed alt vault, client/src/alt/): the game keeps no login name or token of its
// own. A sign-in refreshes a listed account's stored token and makes it the one resumed at start; any
// other sign-in stores nothing and clears that resume pointer. (The Account Manager's Add lists an account
// without a sign-in here: alt_store_list.) Returns 0 (stored, or nothing to store), -1 when the vault could
// not be written.
static int persist_session(const char *username, const char *token) {
    if (!username || !username[0] || !token || !token[0]) return 0;
    if (alt_store_kn_listed(username)) return alt_store_kn_save(username, token);
    return alt_store_kn_sign_out();
}

static void set_session(const char *username, const char *token) {
    copy_str(g_session_user, sizeof(g_session_user), username ? username : "");
    copy_str(g_token, sizeof(g_token), token ? token : "");
}

static void clear_session(void) {
    g_session_user[0] = 0;
    g_token[0] = 0;
}

// A saved session the server refused: its token is forgotten, the account stays listed and asks for
// its password next time.
static void forget_saved_session(void) {
    alt_store_kn_forget(g_session_user);
    clear_session();
    g_restore_retry_at = 0;
}

// Startup: the vault's active listed account becomes the session. The plain keys of older builds
// (krunker_token, krunker_username, krunker_id) are read once: a session they hold moves into the
// list when the vault can be written, and the keys are removed either way, so the game keeps no
// session of its own.
static void load_session(void) {
    clear_session();
    char legacy[sizeof(g_token)], user[sizeof(g_session_user)];
    storage_get("krunker_token", legacy, sizeof(legacy));
    storage_get("krunker_username", user, sizeof(user));
    if (legacy[0] && user[0] && alt_store_ready()) alt_store_kn_save(user, legacy);
    alt_wipe(legacy, sizeof(legacy));
    html_storage_set("krunker_token", NULL);
    html_storage_set("krunker_username", NULL);
    html_storage_set("krunker_id", NULL);
    char name[sizeof(g_session_user)], token[sizeof(g_token)];
    if (alt_store_kn_session(name, sizeof(name), token, sizeof(token)) == 1) set_session(name, token);
    alt_wipe(token, sizeof(token));
}

// readyLoad(): sendAccountRequest(1, [name, null, token]) for a saved session (name and token given)
static void send_restore(const char *name, const char *token) {
    g_restore_sent = 1;
    g_restore_asks++;
    g_restore_at = glfwGetTime();
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("a"));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(1));
    cJSON *payload = cJSON_CreateArray();
    cJSON_AddItemToArray(payload, cJSON_CreateString(name));
    cJSON_AddItemToArray(payload, cJSON_CreateNull());
    cJSON_AddItemToArray(payload, cJSON_CreateString(token));
    cJSON_AddItemToArray(frame, payload);
    backend_social_send_session(frame); // the krunk.io session's upkeep: also while the socket is held
    cJSON_Delete(frame);
}

// The session (g_session_user + g_token) is restored at start and on a new socket. With no session left nothing is
// signed in: a listed account's login is filled in and signed in on the Account Manager's Use (the card's own sign-in).
static void try_restore(void) {
    if (g_restore_sent || !backend_enabled()) return;
    char name[sizeof(g_session_user)], token[sizeof(g_token)];
    copy_str(name, sizeof(name), g_session_user);
    copy_str(token, sizeof(token), g_token);
    if (!name[0] || !token[0]) {
        alt_wipe(token, sizeof(token));
        return;
    }
    send_restore(name, token);
    alt_wipe(token, sizeof(token));
}

static void clear_account(void) {
    g_logged_in = 0;
    store_ui_account_changed();
    clan_ui_reset();
    cJSON_Delete(g_acc.stats);
    cJSON_Delete(g_acc.data);
    cJSON_Delete(g_acc.badges);
    memset(&g_acc, 0, sizeof(g_acc));
    cJSON_Delete(g_chal);
    g_chal = NULL;
    g_chal_requested = 0;
    g_chal_typ[0] = 0;
    cJSON_Delete(g_medal_counts);
    g_medal_counts = NULL;
    cJSON_Delete(g_raid_stats);
    g_raid_stats = NULL;
    g_req_del = 0;
    g_twitch_id[0] = 0;
    g_twitch_name[0] = 0;
    g_promo_emails = -1;
    battlepass_ui_reset(); // windows[5].data / account.BP
    memset(&g_email, 0, sizeof(g_email));
}

// dxp.offer of kn_brain's account info (v10's account object: dxp = {offer: null | {games, xp, claimable},
// doubleClassXP, doubleRaidDrops}). An info object without dxp has no offer; a null info (an "upd" without
// one) keeps what the account had.
static void set_dxp(const cJSON *info) {
    if (!cJSON_IsObject(info)) return;
    const cJSON *dxp = cJSON_GetObjectItem(info, "dxp");
    const cJSON *offer = cJSON_IsObject(dxp) ? cJSON_GetObjectItem(dxp, "offer") : NULL;
    const cJSON *claimable = cJSON_IsObject(offer) ? cJSON_GetObjectItem(offer, "claimable") : NULL;
    g_acc.offer_claimable = claimable && (cJSON_IsTrue(claimable) || (cJSON_IsNumber(claimable) && claimable->valueint));
}

// rrqdxp: the match's score was doubled (v10's RECIEVE_DOUBLE_XP): account.score = playerScore, and the
// offer is spent until the next match (v10's lastDoubleAttempt = games)
void account_double_xp_claimed(const double player_score) {
    if (!g_logged_in) return;
    g_acc.score = player_score;
    update_level();
    g_acc.offer_claimable = 0;
    mark_header();
    rerender_windows();
}

// acc.junk from kn_brain's account info ("junk": the wallet's balance as text, "12.34", doc/v10/junk.md);
// a number is read too, and an info without the key keeps the value it had
static void set_junk(const cJSON *info) {
    const cJSON *junk = cJSON_IsObject(info) ? cJSON_GetObjectItem(info, "junk") : NULL;
    if (!junk) return;
    g_acc.junk = junk_value(cJSON_IsString(junk) ? junk_parse(junk->valuestring) : junk_hundredths(junk->valuedouble));
}

// the "uj" push (social_push_junk): the balance changed while the account is signed in
static void set_junk_text(const char *text) {
    g_acc.junk = junk_value(junk_parse(text));
}

// junk_shop_ui.c: a Junk-priced purchase's reply balance, set as the "uj" push sets it
void account_ui_set_junk(const char *text) {
    if (!g_logged_in || !text) return;
    set_junk_text(text);
    mark_header();
    rerender_windows();
}

// junk_shop_ui.c: an Eterno KR purchase's reply balance (kr), as the account's own refresh would set it
void account_ui_set_funds(const double kr) {
    if (!g_logged_in) return;
    g_acc.funds = kr;
    mark_header();
    rerender_windows();
}

// v10.0.3's account extras from kn_brain's account info (account_life.c): reqDel (setData [29]), twitchId ([44]),
// twitch ([23]) and raidStats ([52]). An info without a key keeps what the account had.
static void set_life(const cJSON *info) {
    if (!cJSON_IsObject(info)) return;
    const cJSON *req = cJSON_GetObjectItem(info, "reqDel");
    if (req) g_req_del = cJSON_IsTrue(req) || (cJSON_IsNumber(req) && req->valueint != 0);
    const cJSON *tw = cJSON_GetObjectItem(info, "twitchId");
    if (tw) copy_str(g_twitch_id, sizeof(g_twitch_id), cJSON_IsString(tw) ? tw->valuestring : "");
    const cJSON *tn = cJSON_GetObjectItem(info, "twitch");
    if (tn) copy_str(g_twitch_name, sizeof(g_twitch_name), cJSON_IsString(tn) ? tn->valuestring : "");
    const cJSON *raids = cJSON_GetObjectItem(info, "raidStats");
    if (cJSON_IsArray(raids)) {
        cJSON_Delete(g_raid_stats);
        g_raid_stats = cJSON_Duplicate(raids, 1);
    }
}

// WP-E part 2c: acc.badges from kn_brain's account info ("badges": badges.js indices); v10's
// creatorBadge (~~creator_badge, v10:112770) comes with it
static void set_badges(const cJSON *info) {
    const cJSON *creator = cJSON_IsObject(info) ? cJSON_GetObjectItem(info, "creator_badge") : NULL;
    if (creator) g_acc.creator = cJSON_IsNumber(creator) ? creator->valueint != 0 : cJSON_IsTrue(creator);
    const cJSON *badges = cJSON_IsObject(info) ? cJSON_GetObjectItem(info, "badges") : NULL;
    if (!cJSON_IsArray(badges)) return;
    cJSON_Delete(g_acc.badges);
    g_acc.badges = cJSON_Duplicate(badges, 1);
}

int account_owns_badge(const int badge) {
    const cJSON *b;
    cJSON_ArrayForEach(b, (g_logged_in ? g_acc.badges : NULL)) {
        if (cJSON_IsNumber(b) && b->valueint == badge) return 1;
    }
    return 0;
}

static int stat_bdg(void) {
    const cJSON *v = g_acc.stats ? cJSON_GetObjectItem(g_acc.stats, "bdg") : NULL;
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

// updateClassPreview: account.stats.bdg >= 0 && Game.badges[bdg] && customBadge && owned
int account_custom_badge(void) {
    if (!g_logged_in || ui_setting_is_false("customBadge")) return -1;
    const int bdg = stat_bdg();
    return badge_get(bdg) && account_owns_badge(bdg) ? bdg : -1;
}

static void add_badge(const int badge) {
    if (account_owns_badge(badge)) return;
    if (!g_acc.badges) g_acc.badges = cJSON_CreateArray();
    cJSON_AddItemToArray(g_acc.badges, cJSON_CreateNumber(badge));
}

// acc.stats[key] = value in both copies (g_acc.stats and data[16])
static void set_stat(const char *key, const double value) {
    if (!g_acc.stats) g_acc.stats = cJSON_CreateObject();
    cJSON_DeleteItemFromObject(g_acc.stats, key);
    cJSON_AddNumberToObject(g_acc.stats, key, value);
    cJSON *stats = g_acc.data ? cJSON_GetArrayItem(g_acc.data, 16) : NULL;
    if (cJSON_IsObject(stats)) {
        cJSON_DeleteItemFromObject(stats, key);
        cJSON_AddNumberToObject(stats, key, value);
    }
}

// accountResponse(html, id, username, data, token)
static void account_response(const cJSON *frame) {
    const cJSON *err = cJSON_GetArrayItem(frame, 1);
    const int was_restore = g_restore_sent;
    g_restore_sent = 0;
    g_auth_sent_at = 0;
    if (cJSON_IsString(err) && err->valuestring[0]) {
        if (was_restore && !g_logged_in && g_restore_asks < RESTORE_ASKS && g_token[0]) {
            // a refused saved session is asked once more (account_ui_tick), before anything is shown or forgotten
            printf("[account] the saved session was refused (%s): asking once more\n", err->valuestring);
            g_restore_retry_at = glfwGetTime() + RESTORE_RETRY_S;
            return;
        }
        if (was_restore) { // a refused saved session is forgotten: the account stays listed with its login (Use fills it in)
            if (g_logged_in) {
                alt_store_kn_forget(g_session_user); // the account signed in here keeps its session in memory
            } else {
                forget_saved_session();
                show_acc_resp(I18N("kn.alt.invalid", "Saved login is no longer valid. Sign in again to restore it."));
            }
            return;
        }
        show_acc_resp(err->valuestring);
        return;
    }
    const cJSON *id = cJSON_GetArrayItem(frame, 2);
    const char *username = str_at(frame, 3);
    const char *token = str_at(frame, 5);
    if (!cJSON_IsNumber(id) || !username) return;

    const int had = g_logged_in;
    clear_account();
    g_logged_in = 1;
    g_restore_asks = 0;
    g_restore_retry_at = 0;
    g_acc.id = id->valueint;
    copy_str(g_acc.name, sizeof(g_acc.name), username);
    copy_str(g_acc.alias, sizeof(g_acc.alias), username);
    set_data(cJSON_GetArrayItem(frame, 4));
    const cJSON *info = cJSON_GetArrayItem(frame, 6); // KrunkNative extra: {level, levelProg, alias, mail_unread, ...}
    if (cJSON_IsObject(info)) {
        const cJSON *mail = cJSON_GetObjectItem(info, "mail_unread");
        if (cJSON_IsNumber(mail)) g_acc.mail_unread = mail->valueint;
        set_badges(info);
        set_dxp(info);
        set_junk(info);
        set_life(info);
    }

    const int have_token = token && token[0];
    if (have_token) {
        set_session(username, token);
        persist_session(username, token); // a listed account keeps its new session
    }
    apply_menu_name();
    mark_header();
    show_acc_resp(NULL);
    if (!had) KN_LOG_DEBUG1("[account] logged in as %s (id %d, level %d, %.0f KR)\n", g_acc.name, g_acc.id, g_acc.level, g_acc.funds);
    // updateAccount -> updateWindow(5); updateWindow(null, true): the open window re-renders
    rerender_windows();
    menu_update_class_box(); // dJ()
    avatar_ui_refresh();     // the profile picture's review status (after the token is saved above)
}

// The signed-in account ends here: the server hears logoutAcc and the session leaves storage. The
// vault keeps the profile (and its session) so the player can switch back to it.
static void end_session(void) {
    send_account_request(-1, NULL, NULL, NULL);
    show_acc_resp(NULL);
    kid_settings_clear(); // the k-ID answer is this account's: the next sign-in asks again
    alt_store_kn_sign_out(); // the listed account stays listed, it is just not the one resumed at start
    clear_session();
    g_tab_index = 0;
    g_logout_sessions = 0;
    clear_account();
    apply_menu_name();
    mark_header();
}

// logoutAcc()
static void logout(void) {
    end_session();
    KN_LOG_DEBUG1("[account] logged out\n");
    menu_show_window(5, 1); // showWindow(5)
    menu_update_class_box();
}

// The Account Manager's Remove: the removed account is signed out first when it is the one signed in.
int account_ui_sign_out_of(const char *username) {
    if (!g_logged_in || !username || strcmp(g_acc.name, username)) return 0;
    end_session();
    KN_LOG_DEBUG1("[account] logged out\n");
    menu_update_class_box();
    return 1;
}

// The saved account the client resumes at start (the listed, active one with its session): 1 and its
// login name, 0 none. The login card and the hub read this instead of a stored login name.
int account_saved_name(char *out, const size_t n) {
    char token[sizeof(g_token)];
    const int found = alt_store_kn_session(out, n, token, sizeof(token));
    alt_wipe(token, sizeof(token));
    if (!found && n) out[0] = 0;
    return found;
}

// The menu's Logout (logoutAcc) and the developer console's `logout` (ui_console.c): 1 when an
// account was logged out, 0 for a guest (nothing done)
int account_ui_logout(void) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) { // the krunker.io account (its tokens only: the krunk.io session stays)
        if (!krunker_account_signed_in()) return 0;
        krunker_account_sign_out();
        KN_LOG_DEBUG1("[account] signed out of krunker.io\n");
        active_tick();
        menu_show_window(5, 1); // showWindow(5): v10's login modal for the guest now
        return 1;
    }
    if (!g_logged_in) return 0;
    logout();
    return 1;
}

static void reward_tick(int force);
static void auth_unreachable(void);
static int progress_frame(const char *label, const cJSON *frame);
static int progress_handle(const char *name, const HtmlArg *args, int argc);
static void medal_tab_html(StrBuf *sb);
static void badge_url(int index, char *out, size_t n);
static void mail_count(int count);
static void mail_read_data(const cJSON *rows);
static void twofa_res(const cJSON *frame);
static void email_frame(const char *label, const cJSON *frame);
static int reset_frame(const char *label, const cJSON *frame);

// v10 REDEEM_VOUCHER_RESPONSE(funds, msg) ("vr", brain redeem.c): msg is the line under the code box;
// funds (set on success) is the account's KR, the header. The granted items come with the "ua" push.
// A vr whose answer never comes (the socket dropped with it) shows an error after its deadline, not "Loading..."
#define VOUCHER_WAIT_MS 15000
static int g_vouch_timer;
static int g_vouch_pending;
static void vouch_expired(void *user) {
    (void) user;
    g_vouch_timer = 0;
    if (!g_vouch_pending) return;
    g_vouch_pending = 0;
    HtmlElem res = html_dom_by_id("vouchRes");
    if (res) html_dom_set_text(res, I18N("kn.vouch.noAnswer", "Couldn't reach the server. Try the code again."));
}

static void redeem_voucher_frame(const cJSON *frame) {
    html_timer_cancel(g_vouch_timer);
    g_vouch_timer = 0;
    g_vouch_pending = 0;
    HtmlElem res = html_dom_by_id("vouchRes");
    if (res) {
        const char *msg = str_at(frame, 2);
        html_dom_set_text(res, msg ? msg : "");
        html_dom_set_display(res, "block");
    }
    const cJSON *funds = cJSON_GetArrayItem(frame, 1);
    if (cJSON_IsNumber(funds)) account_set_funds(funds->valuedouble);
}

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    const char *label = str_at(frame, 0);
    if (!label) return;
    if (!strcmp(label, "__open")) {
        g_restore_asks = 0; // a new socket: the saved session gets its asks again
        g_restore_retry_at = 0;
        if (g_logged_in && g_token[0]) {
            // the socket dropped and came back: authenticate this socket again
            g_restore_sent = 0;
            try_restore();
        } else {
            try_restore();
        }
    } else if (!strcmp(label, "a")) {
        account_response(frame);
    } else if ((!strcmp(label, "ua") || !strcmp(label, "upd")) && g_logged_in) { // ua: updateAccount(data) after a match
        set_data(cJSON_GetArrayItem(frame, 1));
        set_badges(cJSON_GetArrayItem(frame, 2)); // WP-E part 2c: the info's "badges"
        set_dxp(cJSON_GetArrayItem(frame, 2)); // the info's dxp.offer (double_xp_offer.c)
        set_junk(cJSON_GetArrayItem(frame, 2)); // the info's "junk" (doc/v10/junk.md)
        set_life(cJSON_GetArrayItem(frame, 2)); // reqDel / twitch / raidStats (account_life.c)
        mark_header();
        rerender_windows();
    } else if (!strcmp(label, "uj") && g_logged_in) { // social_push_junk: ["uj", "12.34"], the wallet's new balance
        set_junk_text(str_at(frame, 1));
        mark_header();
        rerender_windows();
    } else if (!strcmp(label, "spin") || !strcmp(label, "spinKR") || !strcmp(label, "spinH")) {
        store_ui_frame(frame);
    } else if (!strcmp(label, "vr")) { // v10 REDEEM_VOUCHER_RESPONSE: the redeem popup's answer
        redeem_voucher_frame(frame);
    } else if (!strcmp(label, "ttl")) { // v10: the account's titles (v10_customize.c)
        v10_customize_frame(frame);
    } else if (!strcmp(label, "dsp") || !strcmp(label, "dslr")) { // v10 Daily Spin progress / claim (v10_daily_spin.c)
        v10_daily_spin_frame(label, frame);
    } else if (v10_wild_offer_frame(label, frame)) { // v10 Wild Offers: wo / wob / wor (v10_wild_offer.c)
    } else if (v10_rewards_frame(label, frame)) { // v10.0.2 signup perks: prr (v10_rewards.c)
    } else if (account_backend() != ACCOUNT_BACKEND_KRUNKER && battlepass_ui_frame(label, frame)) { // v7.1.4 Battle Pass: bpdl, bppd, bpclmr, purbp, purbpl, bpEnd (kn_brain's answers: the krunk.io account's, never the krunker.io one; the krunker.io pass comes over the game socket, kio_frame)
    } else if (!strcmp(label, "cln") || !strcmp(label, "clnL")) { // io_cln / io_clnL (windows[12])
        clan_ui_frame(frame);
    } else if (!strcmp(label, "pRes") && !v10_market_owns_frame(label)) { // uploadS reply: popupResponse(kind, error, text)
        const char *kind = str_at(frame, 1);
        popup_response(kind && !strcmp(kind, "SUCCESS") ? 1 : 0, str_at(frame, 2), str_at(frame, 3));
    } else if (!strcmp(label, "loadS")) { // loadSettings(text)
        const char *text = str_at(frame, 1);
        if (text && text[0] && ui_settings_import_text(text)) popup_response(1, NULL, I18N("settings.loaded", "Settings Loaded"));
        else popup_response(0, I18N("settings.loaded.none", "No settings found"), NULL);
        menu_rerender_if_open(1);
    } else if (!strcmp(label, "econN")) {
        // KrunkNative economy note (kn_brain economy.c): no notification on the menu. v10's centre has
        // no economy notes: a gift or a sale is Inbox mail (gmc), a trade shows in the hub's Trades.
    } else if (!strcmp(label, "gmc")) { // updateMailCount(count)
        mail_count((int) num_at(frame, 1));
    } else if (!strcmp(label, "kml")) { // mailListResponse(tab, rows)
        html_timer_cancel(g_mail_timer);
        g_mail_timer = 0;
        g_mail_tab = (int) num_at(frame, 1);
        cJSON_Delete(g_mail_rows);
        const cJSON *rows = cJSON_GetArrayItem(frame, 2);
        g_mail_rows = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : NULL;
        g_mail_loaded = g_mail_rows != NULL;
        g_mail_failed = !g_mail_loaded;
        menu_rerender_if_open(30);
    } else if (!strcmp(label, "rml")) { // readMailResponse(rows)
        mail_read_data(cJSON_GetArrayItem(frame, 1));
    } else if (!strcmp(label, "c2fa")) {
        twofa_res(frame);
    } else if (!strcmp(label, "emailS") || !strcmp(label, "cemail") || !strcmp(label, "emailv")) {
        email_frame(label, frame);
    } else if (reset_frame(label, frame)) { // KrunkNative forgot password: rpr / rpc / rps
    } else if (!strcmp(label, "clm")) { // claimRewardResponse(lastReward, funds)
        if (g_logged_in && g_acc.data) {
            cJSON_ReplaceItemInArray(g_acc.data, 11, cJSON_CreateNumber(num_at(frame, 1)));
            g_acc.funds = num_at(frame, 2);
            mark_header();
            reward_tick(1);
        }
    } else if (!strcmp(label, "nmeR")) { // windows[4].aResp(err, alias)
        const char *err = str_at(frame, 1);
        const char *alias = str_at(frame, 2);
        if (err && err[0]) {
            popup_response(0, err, NULL); // io_pRes(null, err)
        } else {
            popup_response(1, NULL, NULL); // io_pRes("SUCCESS")
            if (alias && g_logged_in) copy_str(g_acc.alias, sizeof(g_acc.alias), alias);
            apply_menu_name();
            mark_header();             // cK()
            menu_update_class_box();   // dJ()
            rerender_windows();        // updateWindow(null, true)
        }
    } else if (!strcmp(label, "flgr")) { // io_flgr(flag): acc.stats.flg; updateWindow(5)
        const cJSON *flag = cJSON_GetArrayItem(frame, 1);
        if (g_logged_in && cJSON_IsNumber(flag)) {
            if (!g_acc.stats) g_acc.stats = cJSON_CreateObject();
            cJSON_DeleteItemFromObject(g_acc.stats, "flg");
            cJSON_AddNumberToObject(g_acc.stats, "flg", flag->valueint);
            if (g_acc.data) {
                cJSON *stats = cJSON_GetArrayItem(g_acc.data, 16);
                if (cJSON_IsObject(stats)) {
                    cJSON_DeleteItemFromObject(stats, "flg");
                    cJSON_AddNumberToObject(stats, "flg", flag->valueint);
                }
            }
        }
        menu_rerender_if_open(5);
    } else if (progress_frame(label, frame)) { // WP-E part 2c: badges, challenges, medals
    } else if (!strcmp(label, "upp")) { // changePasswordResponse(msg, newToken)
        const char *token = str_at(frame, 2);
        if (token && token[0]) {
            set_session(g_acc.name, token);
            persist_session(g_acc.name, token); // a listed account keeps its new session
            apply_menu_name();
        }
        HtmlElem res = html_dom_by_id("newPRes");
        if (res) html_dom_set_html(res, str_at(frame, 1) ? str_at(frame, 1) : "");
    } else if (!strcmp(label, "rqd")) { // deletion request / cancel: [ok, error, null, flag] (account_life.c)
        const int flag = (int) num_at(frame, 4);
        if (num_at(frame, 1) != 1.0 && !cJSON_IsTrue(cJSON_GetArrayItem(frame, 1))) {
            popup_response(0, str_at(frame, 2) ? str_at(frame, 2) : "Server error.", NULL);
        } else if (flag) {
            // v10 logs out after a deletion request (deob-10.0.3.js:244613); kn_brain ends every session too
            account_ui_logout();
            popup_response(1, NULL, I18N("kn.account.delete.requested", "Deletion requested. You have been logged out."));
        } else {
            g_req_del = 0;
            popup_response(1, NULL, I18N("kn.account.delete.cancelled", "Deletion request cancelled."));
            rerender_windows();
        }
    } else if (!strcmp(label, "twitchAuth")) { // the OAuth page to open, or [null, error]
        const char *url = str_at(frame, 1);
        if (url && url[0]) {
            ui_open_browser(url);
            popup_response(1, NULL, I18N("kn.account.twitch.browser", "Finish the Twitch link in your browser, then return here."));
        } else {
            popup_response(0, str_at(frame, 2) ? str_at(frame, 2) : "Twitch linking is currently unavailable.", NULL);
        }
    } else if (!strcmp(label, "twitchVerifyResponse")) { // [error | null, twitchName, twitchId]
        const char *err = str_at(frame, 1);
        if (err) {
            popup_response(0, err, NULL);
        } else {
            copy_str(g_twitch_name, sizeof(g_twitch_name), str_at(frame, 2) ? str_at(frame, 2) : "");
            copy_str(g_twitch_id, sizeof(g_twitch_id), str_at(frame, 3) ? str_at(frame, 3) : "");
            popup_response(1, NULL, I18N("kn.account.twitch.linked", "Twitch account linked."));
            rerender_windows();
        }
    } else if (!strcmp(label, "twitchRemoveResponse")) {
        g_twitch_id[0] = 0;
        g_twitch_name[0] = 0;
        popup_response(1, NULL, I18N("kn.account.twitch.unlinked", "Twitch account unlinked."));
        rerender_windows();
    } else if (!strcmp(label, "twitchDropsResponse")) { // Box Office has no drop campaigns: [] -> "Nothing to Claim"
        const cJSON *drops = cJSON_GetArrayItem(frame, 1);
        popup_response(1, NULL, drops && cJSON_GetArraySize(drops) > 0 ? I18N("kn.account.twitch.drops", "Drops claimed.") : I18N("kn.account.twitch.noDrops", "Nothing to Claim"));
    } else if (!strcmp(label, "__close")) {
        g_restore_sent = 0;
        if (g_auth_sent_at > 0) auth_unreachable();
    }
}

void account_ui_init(Client *client) {
    g_client = client;
    backend_set_brain_gate(brain_gated);
    alt_store_open(NULL); // the vault, if the player has one (no key is read without a vault file)
    alt_manager_ui_init();                     // the krunker.io accounts' events (the Account Manager's rows)
    krunker_net_account_listen(kio_frame);     // the krunker.io account's data (official games)
    backend_social_set_held_sender(kio_held_send);
    load_session();
    if (!backend_enabled()) return;
    backend_social_listen(on_social, NULL);
    backend_social_open(); // "__open" restores the saved session
}

// The daily free KR (config.rewardTime after acc.lastReward): v7.1.4 shows it only as the
// Store's "Free KR Spin" card (store_ui.c), a countdown until it can be claimed. KrunkNative
// also wants one finished match this session (no ads to watch). ms left, 0 = claimable, -1 = none.
double account_reward_ms(void) {
    double last;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) { // the krunker.io account's lastReward (player_lastreward, :114909)
        if (!g_kio.obj) return -1.0;
        last = obj_num("player_lastreward");
    } else {
        if (!g_logged_in || g_acc.level < REWARD_MIN_LVL) return -1.0;
        last = g_acc.data ? num_at(g_acc.data, 11) : 0.0;
    }
    const double db = REWARD_TIME_MS - ((double) time(NULL) * 1000.0 - last);
    return db > 0 ? db : 0.0;
}

static void reward_tick(const int force) {
    if (!g_logged_in || account_reward_ms() < 0) return;
    const double ms = account_reward_ms();
    char t[16];
    snprintf(t, sizeof(t), "%d", ms > 0 ? (int) (ms / 1000) : g_reward_played ? -1 : -2);
    if (!force && !strcmp(t, g_reward_text)) return;
    snprintf(g_reward_text, sizeof(g_reward_text), "%s", t);
    menu_rerender_if_open(14); // the Free KR card's countdown
}

// ---- windows[29]: Krunker Mail

static void mail_count(const int count) {
    g_acc.mail_unread = count < 0 ? 0 : count;
    mark_header();
}

// UTILS.getReadableTime2(ms ago)
static void readable_time2(char *out, const size_t n, const double ms) {
    if (ms < 0) {
        snprintf(out, n, "just now");
        return;
    }
    const long long m = (long long) floor(ms / 60000.0), h = m / 60, d = h / 24;
    if (d) snprintf(out, n, "%lldd ago", d);
    else if (h % 24) snprintf(out, n, "%lldh ago", h % 24);
    else if (m % 60) snprintf(out, n, "%lldm ago", m % 60);
    else snprintf(out, n, "just now");
}

// new Date(text): "YYYY-MM-DD[ T]hh:mm:ss[.frac][Z|+hh[:mm]|-hh[:mm]]" (PostgreSQL / ISO);
// without a zone the time is local, like the browser's parser.
static double parse_date_ms(const char *s) {
    int y, mo, d, h = 0, mi = 0, se = 0, used = 0;
    if (!s || sscanf(s, "%d-%d-%d%*c%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &used) < 6) return 0;
    const char *p = s + used;
    if (*p == '.') while (*++p >= '0' && *p <= '9') {}
    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    if (*p == 'Z') return (double) timegm(&tmv) * 1000.0;
    if (*p == '+' || *p == '-') {
        const int sign = *p == '-' ? -1 : 1;
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 2) sscanf(p + 1, "%2d%2d", &oh, &om);
        return ((double) timegm(&tmv) - sign * (oh * 3600.0 + om * 60.0)) * 1000.0;
    }
    tmv.tm_isdst = -1;
    return (double) mktime(&tmv) * 1000.0;
}

// a JS string argument inside an inline handler attribute (double-quoted attribute, single-quoted arg)
static void mail_arg(StrBuf *sb, const char *s) {
    sb_append(sb, "'");
    for (; s && *s; s++) {
        if (*s == '\'' || *s == '\\') sb_append(sb, "\\");
        if (*s == '"') sb_append(sb, "&quot;");
        else if (*s == '&') sb_append(sb, "&amp;");
        else if (*s == '<') sb_append(sb, "&lt;");
        else sb_append_n(sb, s, 1);
    }
    sb_append(sb, "'");
}

// v10's Followers entry (windows[29].gen, km_notificationtype 0): "X has followed you!" with Follow Back
// (followPlayer, v10_social.c) and the name opening the profile. An unread entry is highlighted like
// v10's unread rows (mailUnread, the 2 px border); the Archive tab shows it plain.
static void mail_follower_row(StrBuf *sb, const int id, const char *ago, const char *name, const int unread) {
    sb_appendf(sb, "<div class='mailObj%s' id='mail_%d' onclick='windows[29].readMail(this, %d, 0)'%s><div style='position:relative;'><div class='mailDate'>%s<div class='followBack' onclick=\"followPlayer(",
               unread ? " mailUnread" : "", id, id, unread ? " style='border: 2px solid #5699eb;'" : "", ago);
    mail_arg(sb, name);
    sb_append(sb, ", this)\">Follow Back</div></div></div><div class='followFrom'><a onclick=\"openPlayerProfile(");
    mail_arg(sb, name);
    sb_append(sb, ")\">");
    sb_append_esc(sb, name);
    sb_append(sb, "</a> has followed you!</div></div>");
}

// A kml whose answer never comes (the socket dropped with it) is failed by its deadline: the list says so, and
// switching the tab asks again.
#define MAIL_WAIT_MS 15000
static void mail_expired(void *user) {
    (void) user;
    g_mail_timer = 0;
    if (g_mail_loaded) return;
    g_mail_failed = 1;
    menu_rerender_if_open(30);
}

static char *gen_mail(void) {
    static const char *const tabs[5] = {"Followers", "Clan", "Hub", "Inbox", "Archive"}; // config.mailTabs
    StrBuf sb = {0};
    sb_append(&sb, "<div>Your Notifications<div class='button buttonR readAll' onclick='windows[29].readAll()'>Read All</div></div><div style='height:30px;'></div><div class='bigMenTabs' style='table-layout: fixed;'>");
    for (int i = 0; i < 5; i++) sb_appendf(&sb, "<div class='bigMenTab%s' onclick='windows[29].switchTab(%d)' style='height:30px;'>%s</div>", g_mail_view == i ? " bigMenTabA" : "", i, tabs[i]);
    sb_append(&sb, "</div><div id='mailList'>");
    if (!g_mail_loaded && g_mail_failed) {
        sb_appendf(&sb, "<span>%s</span>", I18N("kn.mail.loadFailed", "Couldn't load your notifications. Switch tabs to try again."));
    } else if (!g_mail_loaded) {
        char frame[32];
        snprintf(frame, sizeof(frame), "[\"kml\",%d]", g_mail_view == 4 ? 1 : 0);
        html_timer_cancel(g_mail_timer);
        g_mail_timer = html_timer_add(MAIL_WAIT_MS, mail_expired, NULL);
        backend_social_send_text(frame);
        sb_append(&sb, "Loading...");
    } else if (!g_mail_rows) {
        sb_append(&sb, "Loading...");
    } else {
        int shown = 0;
        const cJSON *r;
        const double now = (double) time(NULL) * 1000.0;
        cJSON_ArrayForEach(r, g_mail_rows) {
            const cJSON *archive = cJSON_GetObjectItem(r, "km_archive");
            const int archived = cJSON_IsTrue(archive) || (cJSON_IsNumber(archive) && archive->valuedouble != 0);
            if (archived != (g_mail_view == 4)) continue;
            // km_notificationtype: 0 follow, 1 clan, 2 hub, 3 inbox (4 / 5 count as 3); none = inbox
            const cJSON *type_v = cJSON_GetObjectItem(r, "km_notificationtype");
            int type = cJSON_IsNumber(type_v) ? type_v->valueint : 3;
            if (type == 4 || type == 5) type = 3;
            if (g_mail_view != 4 && type != g_mail_view) continue;
            shown++;
            const int id = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(r, "km_id"));
            char ago[32];
            readable_time2(ago, sizeof(ago), now - parse_date_ms(cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_datesent"))));
            const char *follower = cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_link"));
            if (type == 0 && follower && follower[0]) {
                mail_follower_row(&sb, id, ago, follower, !archived);
                continue;
            }
            sb_appendf(&sb, "<div class='mailObj' id='mail_%d' onclick='windows[29].readMail(this, %d, %d)'><div style='position:relative;'><div class='mailDate'>%s</div></div><div class='mailFrom' style='margin-bottom:15px'>",
                       id, id, type, ago);
            const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_playerfrom"));
            const char *subject = cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_subject"));
            sb_append_esc(&sb, from ? from : "");
            sb_append(&sb, "</div><span class='mailSubj'>");
            sb_append_esc(&sb, subject ? subject : "");
            sb_append(&sb, "</span><div class='mailText'></div></div>");
        }
        if (!shown) sb_append(&sb, "No messages");
        cJSON_Delete(g_mail_rows); // this.mailData = null: the next render asks again
        g_mail_rows = NULL;
        g_mail_loaded = 0;
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// readMail's ["rml", id] has a deadline: a read whose answer never comes (the socket dropped, the brain is gone),
// or that the brain answers with no message (it was deleted), puts the failure text on its row, and clicking the
// row asks again. The answer for the row in flight clears the deadline.
#define MAIL_READ_WAIT_MS 15000
static int g_read_id;    // the message whose rml is in flight (0 none)
static int g_read_timer; // its deadline (html_timer id, 0 none)

static const char *read_failed_text(void) {
    return I18N("kn.mail.readFailed", "Couldn't load this message. Click it again to retry.");
}

static void read_failed(const int id) {
    char el_id[32];
    snprintf(el_id, sizeof(el_id), "mail_%d", id);
    const HtmlElem el = id > 0 ? html_dom_by_id(el_id) : 0;
    const HtmlElem text = el ? html_dom_query(el, ".mailText") : 0;
    if (text) html_dom_set_text(text, read_failed_text());
}

static void read_pending_clear(void) {
    html_timer_cancel(g_read_timer);
    g_read_timer = 0;
    g_read_id = 0;
}

static void read_expired(void *user) {
    (void) user;
    g_read_timer = 0;
    const int id = g_read_id;
    g_read_id = 0;
    read_failed(id);
}

// onReadData([row]): the message text into the opened row
static void mail_read_data(const cJSON *rows) {
    const cJSON *row = cJSON_GetArrayItem(rows, 0);
    if (!row) { // no such message (its row is gone): the row says so instead of "Loading..." for good
        read_failed(g_read_id);
        read_pending_clear();
        return;
    }
    if ((int) cJSON_GetNumberValue(cJSON_GetObjectItem(row, "km_id")) == g_read_id) read_pending_clear();
    char id[32];
    snprintf(id, sizeof(id), "mail_%d", (int) cJSON_GetNumberValue(cJSON_GetObjectItem(row, "km_id")));
    HtmlElem el = html_dom_by_id(id);
    if (el) {
        html_dom_set_style(el, "border", "");
        html_dom_set_class(el, "mailUnread", 0);
        const HtmlElem text = html_dom_query(el, ".mailText");
        const char *km_text = cJSON_GetStringValue(cJSON_GetObjectItem(row, "km_text"));
        StrBuf sb = {0};
        for (const char *p = km_text ? km_text : ""; *p; p++) { // replace(/<|>/g,"").replace("&","") + ASCII only
            if (*p == '<' || *p == '>' || (unsigned char) *p >= 0x80) continue;
            sb_append_n(&sb, p, 1);
        }
        char *clean = sb_finish(&sb);
        if (text) html_dom_set_text(text, clean[0] ? clean : "No message provided");
        free(clean);
    }
    mail_count(g_acc.mail_unread - 1);
}

// ---- 2FA: v7.1.4 index.js twoFAres / f1789 / onInput2FA / onKeyDown2FA / sub2FA / act2FA / dact2FA

// f1789(config.twoFAKeyLen): one digit input per code character
static char *twofa_inputs(void) {
    StrBuf sb = {0};
    for (int i = 0; i < TWOFA_KEY_LEN; i++)
        sb_appendf(&sb, "<input class='twoFAIn' type='number' min='0' max='9' maxlength='1' placeholder=' ' oninput='onInput2FA(this);' onkeydown='onKeyDown2FA(this);' %s>",
                   i == 0 ? "autofocus" : "");
    return sb_finish(&sb);
}

// QrCode.toDataURL(otpUri): an SVG data URL (the HTML engine rasterises SVG with nanosvg)
static char *qr_data_url(const char *text) {
    static QrCode qr;
    if (qr_encode((const unsigned char *) text, strlen(text), QR_ECC_M, -1, &qr) != 0) return NULL;
    static char svg[48 * 1024];
    const int n = qr_svg(&qr, svg, sizeof(svg));
    if (n <= 0) return NULL;
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t len = (size_t) n, cap = 32 + (len + 2) / 3 * 4;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = (size_t) snprintf(out, cap, "data:image/svg+xml;base64,");
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned char) svg[i] << 16 | (i + 1 < len ? (unsigned char) svg[i + 1] << 8 : 0) |
                           (i + 2 < len ? (unsigned char) svg[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63];
        out[o++] = b64[v >> 12 & 63];
        out[o++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? b64[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

// The popup's "Enter Verification Code" line (the second #twoFAmsg of the page)
static HtmlElem twofa_msg_el(void) {
    HtmlElem all[4];
    const int n = html_dom_query_all(0, "#twoFAmsg", all, 4);
    return n > 0 ? all[n - 1] : 0;
}

// twoFAres(status, nextLabel, otpUri, twoFAVal) + KrunkNative's message (6th element)
static void twofa_res(const cJSON *frame) {
    const int status = (int) num_at(frame, 1);
    const char *label = str_at(frame, 2);
    const char *uri = str_at(frame, 3);
    const char *msg = str_at(frame, 5);
    copy_str(g_2fa_label, sizeof(g_2fa_label), label);
    ui_loadout_clear_pops(); // clearPops()
    if (status) {
        if (status == 1) {
            set_display("twoFAPop", "none");
        } else if (status == 2 && g_logged_in) {
            const cJSON *val = cJSON_GetArrayItem(frame, 4);
            g_acc.has_2fa = cJSON_IsTrue(val) || (cJSON_IsNumber(val) && val->valuedouble != 0);
            set_display("twoFAPop", "none"); // KrunkNative: the confirm popup closes too
            menu_rerender_if_open(5);         // updateWindow(5)
        }
        return;
    }
    set_display("twoFAPop", "block");
    HtmlElem btn = html_dom_by_id("twoFAbtn");
    if (btn) {
        html_dom_set_html(btn, "Submit");
        html_dom_set_attr(btn, "class", "twoFAbtn");
    }
    HtmlElem form = html_dom_by_id("twoFAForm");
    char *inputs = twofa_inputs();
    if (form) html_dom_set_html(form, inputs);
    free(inputs);
    HtmlElem first = form ? html_dom_query(form, ".twoFAIn") : 0;
    if (first) html_dom_focus(first);
    HtmlElem qr = html_dom_by_id("twoFAqr");
    if (uri && qr) {
        char *url = qr_data_url(uri);
        HtmlElem img = html_dom_query(qr, "img");
        if (url && img) {
            html_dom_set_attr(img, "src", url);
            html_dom_set_display(qr, "block");
        }
        free(url);
    } else if (qr && (!label || strcmp(label, "a2FAc") != 0)) {
        html_dom_set_display(qr, "none"); // v7 leaves the setup QR up for later prompts; KrunkNative hides it
    }
    HtmlElem m = twofa_msg_el();
    if (m) {
        html_dom_set_text(m, msg && msg[0] ? msg : "Enter Verification Code");
        html_dom_set_style(m, "color", msg && msg[0] ? "#e33b3b" : "");
    }
}

// The code typed so far (only single digits count, like sub2FA)
static int twofa_code(char out[TWOFA_KEY_LEN + 1]) {
    HtmlElem ins[TWOFA_KEY_LEN];
    const int n = html_dom_query_all(0, ".twoFAIn", ins, TWOFA_KEY_LEN);
    int len = 0;
    for (int i = 0; i < n && i < TWOFA_KEY_LEN; i++) {
        char v[16] = "";
        html_dom_get_value(ins[i], v, sizeof(v));
        if (strlen(v) == 1 && isdigit((unsigned char) v[0])) out[len++] = v[0];
    }
    out[len] = 0;
    return len;
}

// sub2FA(): io.send(v10808 || "2fa", code) once every digit is there
static void twofa_submit(void) {
    char code[TWOFA_KEY_LEN + 1];
    if (twofa_code(code) != TWOFA_KEY_LEN) return;
    HtmlElem btn = html_dom_by_id("twoFAbtn");
    if (btn) {
        html_dom_set_html(btn, "...");
        html_dom_set_attr(btn, "class", "twoFAbtnN");
    }
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString(g_2fa_label[0] ? g_2fa_label : "2fa"));
    cJSON_AddItemToArray(frame, cJSON_CreateString(code));
    backend_social_send(frame);
    cJSON_Delete(frame);
}

// previousElementSibling / nextElementSibling among the popup's inputs
static HtmlElem twofa_sibling(const HtmlElem el, const int dir) {
    HtmlElem ins[TWOFA_KEY_LEN];
    const int n = html_dom_query_all(0, ".twoFAIn", ins, TWOFA_KEY_LEN);
    for (int i = 0; i < n; i++)
        if (ins[i] == el) return i + dir >= 0 && i + dir < n ? ins[i + dir] : 0;
    return 0;
}

// onInput2FA(this): one digit per box, the caret moves on; the last box submits
static void twofa_on_input(const HtmlElem el) {
    if (!el) return;
    char v[32] = "";
    html_dom_get_value(el, v, sizeof(v));
    const size_t len = strlen(v);
    if (len == 0) {
        const HtmlElem prev = twofa_sibling(el, -1);
        if (prev) html_dom_focus(prev);
        return;
    }
    if (len > 1) {
        const char last[2] = {v[len - 1], 0};
        html_dom_set_value(el, last);
    }
    const HtmlElem next = twofa_sibling(el, 1);
    if (next) html_dom_focus(next);
    else twofa_submit(); // twoFAbtn.click()
}

// onKeyDown2FA(this): Enter submits a complete code; Backspace clears and steps back
static int twofa_on_keydown(const HtmlElem el, const int key) {
    if (html_key_is_enter(key)) {
        char code[TWOFA_KEY_LEN + 1];
        if (twofa_code(code) >= TWOFA_KEY_LEN) twofa_submit();
        return 0;
    }
    if (key == 259 || key == 8) { // GLFW_KEY_BACKSPACE natively, keyCode 8 on the web
        char v[16] = "";
        if (el) html_dom_get_value(el, v, sizeof(v));
        const HtmlElem prev = el ? twofa_sibling(el, -1) : 0;
        if (!v[0] && prev) {
            html_dom_focus(prev);
            html_dom_set_value(prev, "");
        } else if (el) {
            html_dom_set_value(el, "");
        }
        return 0;
    }
    return 1;
}

// ---- KrunkNative email verification (Account Security)

static void email_res(const char *text, const int error) {
    HtmlElem el = html_dom_by_id("emailRes");
    if (!el) return;
    html_dom_set_text(el, text ? text : "");
    html_dom_set_style(el, "color", error ? "#e33b3b" : "rgba(255,255,255,0.5)");
}

static void email_state(const cJSON *st) {
    if (!cJSON_IsObject(st)) return;
    const char *addr = cJSON_GetStringValue(cJSON_GetObjectItem(st, "email"));
    copy_str(g_email.addr, sizeof(g_email.addr), addr);
    g_email.verified = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "verified"));
    g_email.pending = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "pending"));
    g_email.wait = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "wait"));
    g_email.mail = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "mail"));
    g_email.known = 1;
}

// ["emailS", state] / ["cemail" | "emailv", error, message, state]
static void email_frame(const char *label, const cJSON *frame) {
    if (!g_logged_in) return;
    if (!strcmp(label, "emailS")) {
        email_state(cJSON_GetArrayItem(frame, 1));
        if (g_tab_index == PROFILE_TAB_SETTINGS) menu_rerender_if_open(5);
        return;
    }
    const char *err = str_at(frame, 1), *msg = str_at(frame, 2);
    email_state(cJSON_GetArrayItem(frame, 3));
    if (g_tab_index == PROFILE_TAB_SETTINGS) menu_rerender_if_open(5);
    email_res(err ? err : msg, err != NULL);
}

// The Account Security block: the address, its state, change / resend
static void email_html(StrBuf *sb) {
    sb_append(sb, "<div class='settName' style='margin-bottom:20px;'></div><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px;margin-bottom: 10px;'>");
    if (g_email.addr[0]) {
        sb_appendf(sb, "%s: <span style='color:#fff'>", I18N("generic.email", "Email"));
        sb_append_esc(sb, g_email.addr);
        sb_appendf(sb, "</span> <span style='color:%s'>%s</span>", g_email.verified ? "#3be354" : "#e33b3b",
                   g_email.verified ? I18N("kn.account.emailVerified", "Verified") : I18N("kn.account.emailNotVerified", "Not verified"));
        if (!g_email.verified && g_email.mail)
            sb_appendf(sb, "<div class='subPassChg' onclick='resendEmail()' style='float: right;margin-top: -8px;'>%s</div>", I18N("kn.account.emailResend", "Resend"));
    } else {
        char add[192];
        I18N_F(add, "kn.account.emailAdd", "Add an {0}Email{1} to verify your account", "<span style='color:#fff'>", "</span>");
        sb_append(sb, add);
    }
    sb_append(sb, "</div><input type='email' id='accEmailNew' class='smlInput' style='width:82%;margin-bottom:8px' placeholder='");
    sb_append(sb, g_email.addr[0] ? I18N("generic.newEmail", "New Email") : I18N("generic.email", "Email"));
    sb_appendf(sb, "' /><div class='subPassChg' onclick='changeEmail()'>%s</div><div id='emailRes' style='color:rgba(255,255,255,0.5)'>", I18N("generic.submit", "Submit"));
    if (!g_email.mail && g_email.known) sb_append(sb, I18N("kn.account.emailUnavailable", "Verification emails are unavailable on this server"));
    else if (!g_email.verified && g_email.pending) sb_append(sb, I18N("kn.account.emailCheckInbox", "Check your inbox for the verification link"));
    sb_append(sb, "</div>");
}

// The active account changed (backend_ui.h: an official game joined or left, either backend's
// sign-in or sign-out): everything that shows the account follows at once, as after a login.
static void active_changed(void) {
    g_active.generation++;
    battlepass_ui_reset(); // windows[5].data, account.BP: the other account's
    clan_ui_reset();       // clanData: the other account's
    apply_menu_name();
    update_header();       // v10_menu_account_changed(): the header bars, the nav, the login modal
    rerender_windows();    // the Account window, Battle Pass, Store, ... of the open window
    menu_update_class_box();
}

static void active_tick(void) {
    battlepass_ui_tick(); // a "bpdl" unanswered for too long is asked again (no failure text: battlepass_ui.c)
    const int backend = (int) account_backend();
    const int kio = krunker_account_signed_in();
    const unsigned kio_gen = krunker_account_generation();
    if (backend == g_active.backend && g_logged_in == g_active.kn_logged_in && kio == g_active.kio_signed_in &&
        (backend != ACCOUNT_BACKEND_KRUNKER || (kio_gen == g_active.kio_gen && g_kio.gen == g_active.kio_data_gen)))
        return;
    const int first = g_active.backend < 0;
    if (!first && backend == ACCOUNT_BACKEND_KRUNKER && g_active.backend == ACCOUNT_BACKEND_KRUNKER && kio && !g_active.kio_signed_in)
        krunker_net_account_signed_in(); // signed in from the in-game menu: this game's socket too
    // every window logs the backend it starts on and each change, with its pid: stderr, as the window
    // processes' own diagnostics are (a child window's stdout is buffered when it goes to a file)
    if (backend != g_active.backend)
        fprintf(stderr, "[account] active account backend: %s (pid %ld)\n", backend == ACCOUNT_BACKEND_KRUNKER ? "krunker.io" : "krunk.io",
                process_id());
    g_active.backend = backend;
    // a krunker.io game: nothing done under its account reaches the krunk.io account (backend.h)
    backend_social_hold(backend == ACCOUNT_BACKEND_KRUNKER);
    g_active.kn_logged_in = g_logged_in;
    g_active.kio_signed_in = kio;
    g_active.kio_gen = kio_gen;
    const int data_only = !first && backend == ACCOUNT_BACKEND_KRUNKER && g_active.kio_data_gen != g_kio.gen && kio_gen == g_active.kio_gen &&
                          kio == g_active.kio_signed_in;
    g_active.kio_data_gen = g_kio.gen;
    if (data_only) { // the krunker.io account's KR / skins / stats moved: the header and the open window follow
        g_active.generation++;
        update_header();
        rerender_windows();
        return;
    }
    if (!first) active_changed();
    else g_active.generation++;
}

void account_ui_tick(Client *client) {
    store_ui_tick(); // the wheel screen; also without a brain (a --render-scenario's state.spin)
    active_tick();
    if (!backend_enabled()) return;
    // a krunker.io game is not a krunk.io match: its end neither counts toward the krunk.io
    // account's daily reward nor refreshes it (a krunker.io session never touches that account)
    const int krunkio = account_backend() == ACCOUNT_BACKEND_KRUNKIO;
    // a match ended (Krunker "end": canClaimReward) -> dc
    static int reward_end = -1;
    const int end_now = krunkio && client->net_end_seconds >= 0;
    if (end_now && reward_end == 0 && g_logged_in) {
        g_reward_played = 1;
        reward_tick(1);
    }
    reward_end = end_now;
    reward_tick(0);
    if (g_auth_sent_at > 0 && !g_auth_captcha && glfwGetTime() - g_auth_sent_at > AUTH_TIMEOUT_SECONDS) auth_unreachable();
    // the saved session's asks: a refused one is asked again (account_response), an unanswered one after a while
    if (g_restore_retry_at > 0 && glfwGetTime() >= g_restore_retry_at) {
        g_restore_retry_at = 0;
        if (!g_logged_in && g_token[0] && backend_social_is_open()) {
            g_restore_sent = 0;
            try_restore();
        }
    }
    if (g_restore_sent && !g_logged_in && g_token[0] && g_restore_asks < RESTORE_ASKS && backend_social_is_open() &&
        glfwGetTime() - g_restore_at > RESTORE_ANSWER_S) {
        printf("[account] the saved session has no answer yet: asking again\n");
        g_restore_sent = 0;
        try_restore();
    }
    if (g_header_dirty) update_header();
    // A round ended: the game server reported the match to the brain. Refresh the
    // account (KR, score, level) with a token login a moment later, like v3.1.1's
    // updateAccount after the end screen (unless the brain pushed "upd" already).
    static int last_end = -1;
    static double refresh_at;
    const int end = krunkio && client->net_end_seconds >= 0;
    if (end && !last_end && g_logged_in) refresh_at = glfwGetTime() + 3.0;
    last_end = end;
    if (refresh_at > 0 && glfwGetTime() >= refresh_at) {
        refresh_at = 0;
        if (g_logged_in && g_token[0] && backend_social_is_open()) {
            g_restore_sent = 0;
            try_restore();
        }
    }
}

// ------------------------------------------------------------------ handlers

static void read_input(const char *id, char *out, const size_t n) {
    out[0] = 0;
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_get_value(el, out, n);
}

// No account server answering. v3.1.1 only says "Please try again." in #accResp; KrunkNative
// names the reason, so a player can tell offline mode, a down server and no connection apart
// (the details also go to the log).
static void auth_unreachable(void) {
    const char *msg;
    if (!online_mode_online()) {
        msg = I18N("kn.account.offlineMode", "Accounts are off in Offline Mode. Switch Account Mode to Online in Settings &gt; Client and restart.");
        printf("[account] offline mode: no account service\n");
    } else if (!backend_enabled()) {
#ifdef _WIN32
        msg = I18N("kn.account.notInBuild", "Accounts are not available in this build yet. Please try again after the next update.");
#else
        msg = I18N("kn.account.noServer", "No account server is set. Check Settings &gt; Client &gt; Matchmaker URL.");
#endif
        printf("[account] no account server configured (--brain / KRUNKNATIVE_BRAIN_URL / knBrainUrl)\n");
    } else if (backend_available() == 0) {
        msg = I18N("kn.account.unreachable", "Can't reach the account server. Check your internet connection and try again.");
        printf("[account] no account server at %s (GET /mm/status found no kn_brain)\n", backend_url());
    } else if (!backend_social_is_open()) {
        msg = I18N("kn.account.unreachable", "Can't reach the account server. Check your internet connection and try again.");
        printf("[account] the social socket to %s did not open\n", backend_url());
    } else {
        msg = I18N("kn.account.noAnswer", "The account server did not answer. Please try again.");
        printf("[account] no answer from %s within %.0f s\n", backend_url(), AUTH_TIMEOUT_SECONDS);
    }
    show_acc_resp(msg);
    g_auth_sent_at = 0;
}

// ---- KrunkNative password reset (the login popup's recover card, v10_login.c recover())
//
// kn_brain mails a one-time code to the account's verified email (services/brain/
// account_security.c): ["rpr", name | email] -> ["rpr", err | null, msg]; ["rpc", code] ->
// ["rpc", err | null, username]; ["rps", code, new, repeat] -> ["rps", err | null, msg,
// username, token]. Step 1 asks for the name or email, step 2 for the code (pasted or typed;
// the mail's link opens the same form on the brain's site instead), step 3 for the new
// password; a success signs in with the returned session, like a login.

static struct {
    int step;          // 0 = the login form, 1 = name / email, 2 = code, 3 = new password
    char code[32];     // the checked code (step 3)
    char user[64];     // its account
    char note[320];    // the line under the fields (escaped HTML)
    int note_error;
} g_reset;

static void reset_note(const char *text, const int error) {
    HtmlElem el = html_dom_by_id("knResetRes");
    copy_str(g_reset.note, sizeof(g_reset.note), text);
    g_reset.note_error = error;
    if (!el) return;
    html_dom_set_text(el, text ? text : "");
    html_dom_set_style(el, "color", error ? "#e33b3b" : "rgba(255,255,255,0.5)");
}

// The reset steps over the Account window's guest form (window 5's #menuWindow content)
static void reset_render(void) {
    HtmlElem win = html_dom_by_id("menuWindow");
    if (!win || g_reset.step <= 0) return;
    StrBuf sb = {0};
    sb_appendf(&sb, "<div style='position:relative;z-index:9'><div id='referralHeader'>%s</div><div style='height:20px;'></div>",
               I18N("kn.account.reset.header", "Reset Password"));
    if (g_reset.step == 1) {
        sb_appendf(&sb, "<div style='font-size:18px;color:rgba(255,255,255,0.5);margin-bottom:12px'>%s</div>"
                        "<input id='knResetWho' type='text' placeholder='%s' class='accountInput' style='margin-top:0'>",
                   I18N("kn.account.reset.askWho", "Enter your username or the verified email of your account. We'll email you a reset code."),
                   I18N("kn.account.reset.who", "Username or Email"));
    } else if (g_reset.step == 2) {
        sb_appendf(&sb, "<div style='font-size:18px;color:rgba(255,255,255,0.5);margin-bottom:12px'>%s</div>"
                        "<input id='knResetCode' type='text' placeholder='%s' class='accountInput' style='margin-top:0' "
                        "autocomplete='one-time-code'>",
                   I18N("kn.account.reset.askCode", "Enter the code from the email (XXXX-XXXX-XXXX). It works once and expires after 30 minutes."),
                   I18N("kn.account.reset.code", "Reset Code"));
    } else {
        StrBuf user = {0};
        sb_append(&user, "<span style='color:#fff'>");
        sb_append_esc(&user, g_reset.user);
        sb_append(&user, "</span>");
        char *u = sb_finish(&user);
        char line[512];
        I18N_F(line, "kn.account.reset.choose", "Choose a new password for {0}.", u);
        free(u);
        sb_appendf(&sb, "<div style='font-size:18px;color:rgba(255,255,255,0.5);margin-bottom:12px'>%s</div>"
                        "<input id='knResetPass' type='password' placeholder='%s' class='accountInput' style='margin-top:0'>"
                        "<input id='knResetPass2' type='password' placeholder='%s' class='accountInput'>",
                   line, I18N("kn.account.reset.newPass", "New Password"), I18N("kn.account.reset.repeatPass", "Repeat Password"));
    }
    sb_appendf(&sb, "<div class='setBodH' style='margin-left:0px;width:calc(100%% - 40px)'><div id='knResetRes' style='margin-top:20px;"
                    "margin-bottom:20px;font-size:18px;color:%s;text-align:center'>",
               g_reset.note_error ? "#e33b3b" : "rgba(255,255,255,0.5)");
    sb_append_esc(&sb, g_reset.note);
    sb_appendf(&sb, "</div></div><div style='width:100%%;text-align:center;margin-top:10px;background-color:rgba(0,0,0,0.3);padding-top:10px;padding-bottom:20px;'>"
                    "<div class='accBtn button buttonG' onclick='knResetBack()'>%s</div>", I18N("generic.back", "Back"));
    sb_appendf(&sb, "<div class='accBtn button buttonP' id='knResetGo' onclick='knResetNext()'>%s</div>",
               g_reset.step == 1 ? I18N("kn.account.reset.send", "Send Code") : g_reset.step == 2 ? I18N("kn.account.reset.continue", "Continue")
                                 : I18N("kn.account.reset.set", "Set Password"));
    if (g_reset.step == 1) {
        char line[384];
        I18N_F(line, "kn.account.reset.haveCode", "Got a code already? {0}Enter it{1}",
               "<span id='knResetHave' onclick='window.knResetHave()' style='color:rgba(255,255,255,0.8);cursor:pointer;text-decoration:underline'>", "</span>");
        sb_appendf(&sb, "<div style='margin-top:14px;font-size:16px;color:rgba(255,255,255,0.5)'>%s</div>", line);
    }
    if (g_reset.step == 2) {
        char line[384];
        I18N_F(line, "kn.account.reset.noEmail", "No email? {0}Send another code{1}",
               "<span id='knResetResend' onclick='window.knResetResend()' style='color:rgba(255,255,255,0.8);cursor:pointer;text-decoration:underline'>", "</span>");
        sb_appendf(&sb, "<div style='margin-top:14px;font-size:16px;color:rgba(255,255,255,0.5)'>%s</div>", line);
    }
    sb_append(&sb, "</div></div>");
    html_dom_set_html(win, sb.data ? sb.data : "");
    free(sb.data);
    const char *focus = g_reset.step == 1 ? "knResetWho" : g_reset.step == 2 ? "knResetCode" : "knResetPass";
    HtmlElem f = html_dom_by_id(focus);
    if (f) html_dom_focus(f);
}

int account_ui_reset_active(void) { return g_reset.step > 0; }

static void reset_open(const int step, const char *note, const int error) {
    g_reset.step = step;
    copy_str(g_reset.note, sizeof(g_reset.note), note);
    g_reset.note_error = error;
    if (step <= 0) {
        memset(&g_reset, 0, sizeof(g_reset));
        menu_rerender_if_open(5); // the login form again
        return;
    }
    reset_render();
}

static void reset_send(const char *label, const char *a, const char *b, const char *c) {
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString(label));
    cJSON_AddItemToArray(frame, cJSON_CreateString(a ? a : ""));
    if (b) cJSON_AddItemToArray(frame, cJSON_CreateString(b));
    if (c) cJSON_AddItemToArray(frame, cJSON_CreateString(c));
    backend_social_send(frame);
    cJSON_Delete(frame);
}

static char g_reset_who[256]; // step 1's input, for "Send another code"

static void reset_next(void) {
    if (!backend_enabled() || backend_available() == 0) {
        auth_unreachable();
        HtmlElem res = html_dom_by_id("accResp");
        char text[320] = "";
        if (res) html_dom_get_text(res, text, sizeof(text));
        reset_note(text[0] ? text : I18N("custom.tryagain", "Please try again."), 1);
        return;
    }
    if (g_reset.step == 1) {
        read_input("knResetWho", g_reset_who, sizeof(g_reset_who));
        if (!g_reset_who[0]) {
            reset_note(I18N("kn.account.reset.needWho", "Enter your username or email"), 1);
            return;
        }
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rpr", g_reset_who, NULL, NULL);
    } else if (g_reset.step == 2) {
        char code[64];
        read_input("knResetCode", code, sizeof(code));
        if (!code[0]) {
            reset_note(I18N("kn.account.reset.needCode", "Enter the code from the email"), 1);
            return;
        }
        copy_str(g_reset.code, sizeof(g_reset.code), code);
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rpc", code, NULL, NULL);
    } else if (g_reset.step == 3) {
        char p1[128], p2[128];
        read_input("knResetPass", p1, sizeof(p1));
        read_input("knResetPass2", p2, sizeof(p2));
        if (!p1[0] || strcmp(p1, p2) != 0) {
            reset_note(p1[0] ? I18N("kn.account.reset.mismatch", "Passwords do not match.") : I18N("kn.account.reset.needPass", "Enter a new password"), 1);
            return;
        }
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rps", g_reset.code, p1, p2);
        memset(p1, 0, sizeof(p1));
        memset(p2, 0, sizeof(p2));
    }
}

// ["rpr" | "rpc" | "rps", error | null, message, ...]
static int reset_frame(const char *label, const cJSON *frame) {
    const int rpr = !strcmp(label, "rpr"), rpc = !strcmp(label, "rpc"), rps = !strcmp(label, "rps");
    if (!rpr && !rpc && !rps) return 0;
    if (g_reset.step <= 0) return 1;
    const char *err = str_at(frame, 1), *msg = str_at(frame, 2);
    if (err) {
        reset_note(err, 1);
        return 1;
    }
    if (rpr) {
        reset_open(2, msg ? msg : I18N("kn.account.reset.checkInbox", "Check your inbox."), 0);
    } else if (rpc) {
        copy_str(g_reset.user, sizeof(g_reset.user), msg);
        reset_open(3, "", 0);
    } else {
        // signed in with the new session: the same restore as a saved login
        const char *user = str_at(frame, 3), *token = str_at(frame, 4);
        memset(&g_reset, 0, sizeof(g_reset));
        if (user && user[0] && token && token[0]) {
            set_session(user, token);
            persist_session(user, token);
            g_restore_sent = 0;
            try_restore();
        }
        menu_rerender_if_open(5);
        show_acc_resp(msg ? msg : I18N("kn.account.reset.updated", "Password updated."));
        ui_notify(msg ? I18N("kn.account.reset.updatedLoggedIn", "Password updated. You are now logged in.") : I18N("kn.account.reset.updated", "Password updated."));
    }
    return 1;
}

int account_ui_reset_enter(const HtmlElem target) {
    if (g_reset.step <= 0 || !target) return 0;
    static const char *const ids[] = {"knResetWho", "knResetCode", "knResetPass", "knResetPass2"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (target == html_dom_by_id(ids[i])) {
            if (target == html_dom_by_id("knResetPass")) {
                HtmlElem next = html_dom_by_id("knResetPass2");
                if (next) html_dom_focus(next);
            } else {
                reset_next();
            }
            return 1;
        }
    }
    return 0;
}

// knResetOpen / knResetNext / knResetBack / knResetHave / knResetResend (onclick names C writes)
static int reset_handle(const char *name) {
    if (!strcmp(name, "knResetOpen")) {
        char who[64];
        read_input("accName", who, sizeof(who));
        reset_open(1, "", 0);
        HtmlElem in = html_dom_by_id("knResetWho");
        if (in && who[0]) html_dom_set_value(in, who);
        return 1;
    }
    if (!strcmp(name, "knResetNext")) {
        reset_next();
        return 1;
    }
    if (!strcmp(name, "knResetBack")) {
        reset_open(g_reset.step >= 2 ? g_reset.step - 1 : 0, "", 0);
        return 1;
    }
    if (!strcmp(name, "knResetHave")) { // a code from an earlier request (the game was restarted meanwhile)
        reset_open(2, "", 0);
        return 1;
    }
    if (!strcmp(name, "knResetResend")) {
        if (!g_reset_who[0]) {
            reset_open(1, "", 0);
            return 1;
        }
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rpr", g_reset_who, NULL, NULL);
        return 1;
    }
    return 0;
}

int account_ui_twofa_handler(const char *name) {
    return name && (!strcmp(name, "sub2FA") || !strcmp(name, "onInput2FA") || !strcmp(name, "onKeyDown2FA"));
}

static int same_login_text(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (tolower((unsigned char) *a) != tolower((unsigned char) *b)) return 0;
    }
    return *a == *b;
}

// The login card's Login names an account other than the signed-in one: a sign-in would switch the session (the
// reply's account replaces this one). The identity held decides. A name is compared with the signed-in account's
// name. An address (typed in either field: the brain signs in by it) is compared with the address on file, which the
// brain pushes after every login (emailS). Until that address is known nothing shows the login is this account, so
// it is refused (the fallback), and the card says whose session it is.
static int names_other_account(const char *user, const char *mail) {
    if (!g_logged_in) return 0;
    const char *ident = user[0] ? user : mail;
    if (!ident[0]) return 0;
    if (!strchr(ident, '@')) return !same_login_text(ident, g_acc.name);
    if (!g_email.known) return 1; // the fallback: no address held yet
    return !(g_email.addr[0] && same_login_text(ident, g_email.addr));
}

// The Account Manager's Use (alt_manager_ui.c): a Box Office account other than `login`'s is signed in, by the
// card's own comparison, so the row's sign-in logs it out first (account_ui_logout).
int account_ui_other_signed_in(const char *login) {
    return login && names_other_account(login, "");
}

// The card's message (accResp): the signed-in account stays, and the login typed is not signed in. Nothing is sent.
static void show_other_signed_in(const char *typed) {
    char text[300];
    I18N_F(text, "kn.account.otherSignedIn", "Signed in as {0}. Log out to sign in as {1}.", g_acc.name, typed);
    StrBuf sb = {0};
    sb_append_esc(&sb, text);
    char *html = sb_finish(&sb);
    show_acc_resp(html);
    free(html);
}

// ------------------------------------------------------------------ Account Privacy / Twitch (kn_brain account_life.c)

// v10's requestDeletion(flag) -> rpc("rqd", flag) (deob-10.0.3.js:244600). The request itself is confirmed first;
// cancelling needs none.
static void deletion_confirmed(const int ok, const char *text, void *user) {
    (void) text;
    if (!ok || !g_logged_in) return;
    char frame[24];
    snprintf(frame, sizeof frame, "[\"rqd\",%d]", (int) (intptr_t) user);
    backend_social_send_text(frame);
}

// v10's connectTwitchAcc: the confirm, then the OAuth page. Here kn_brain is the OAuth client: "twitchAuth"
// answers the page to open in the system browser (account_life.c), and the callback pushes twitchVerifyResponse.
static void twitch_link_confirmed(const int ok, const char *text, void *user) {
    (void) text;
    (void) user;
    if (!ok || !g_logged_in) return;
    backend_social_send_text("[\"twitchAuth\"]");
}

static void twitch_unlink_confirmed(const int ok, const char *text, void *user) {
    (void) text;
    (void) user;
    if (!ok || !g_logged_in) return;
    backend_social_send_text("[\"removeTwitchLink\"]");
}


// windows[4].collapseCon(hd) (deob-10.0.3.js:247458-247463): setHed_<x> shows / hides setBod_<x>, and its first
// child (the plusOrMinus3 chevron) reads keyboard_arrow_down / _right
static void collapse_con(const HtmlElem hd) {
    char id[64], body_id[72];
    if (!hd || html_dom_get_attr(hd, "id", id, sizeof(id)) < 0 || strncmp(id, "setHed_", 7)) return;
    snprintf(body_id, sizeof(body_id), "setBod_%s", id + 7);
    const HtmlElem body = html_dom_by_id(body_id), icon = html_dom_first_child(hd);
    if (!body) return;
    char display[16] = "";
    const int closed = html_dom_get_style(body, "display", display, sizeof(display)) >= 0 && !strcmp(display, "none");
    if (icon) html_dom_set_html(icon, closed ? "keyboard_arrow_down" : "keyboard_arrow_right");
    html_dom_set_display(body, closed ? "" : "none");
}

// email.setPreference({promotional, source}) (deob-10.0.3.js:130868-130882): a 200 with data.success caches
// {chosen: true, promotional}; anything else leaves the cache (togglePromoEmails reads no answer). kn_brain's POST
// /email/preferences refuses (v10_auth.c: Box Office stores no email preference), so the row stays unchecked
// the next time the window draws, as v10's does when the call fails.
static void promo_emails_done(void *user, const int status, const char *body, const size_t len) {
    const int on = (int) (intptr_t) user;
    cJSON *j = status == 200 && body ? cJSON_ParseWithLength(body, len) : NULL;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(j, "data"), "success"))) g_promo_emails = on;
    cJSON_Delete(j);
}

static void promo_emails_set(const int on) {
    char url[512];
    snprintf(url, sizeof(url), "%s/email/preferences", backend_url());
    if (!backend_enabled() || !backend_http_request("POST", url, on ? "{\"promotional\":true,\"source\":\"settings\"}" : "{\"promotional\":false,\"source\":\"settings\"}",
                                                    promo_emails_done, (void *) (intptr_t) on))
        printf("[account] togglePromoEmails: no account service\n");
}

// ---- the Free KR card's ad (claimReward): its AD_STARTED / AD_LOADED / AD_FAILED to the account's own socket (the krunker.io
// game socket, or kn_brain's social socket, which takes them as no-ops), then the wheel
static void free_kr_ad_frame(const char *label) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) {
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString(label));
        krunker_net_send_account_frame(frame);
        cJSON_Delete(frame);
        return;
    }
    char text[24];
    snprintf(text, sizeof text, "[\"%s\"]", label);
    backend_social_send_text(text);
}
static void free_kr_ad_shown(void *user) {
    (void) user;
    free_kr_ad_frame("lfkr");
}
static void free_kr_ad_done(void *user) {
    (void) user;
    store_ui_free_kr_wheel(); // prizeWheel(4): its spin sends "fkr"
}
static void free_kr_ad_error(void *user, const char *error) {
    (void) user;
    (void) error; // "skipped": v7 shows nothing for it (the only error the placeholder has)
    free_kr_ad_frame("fkrf");
}

int account_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    // No account service, or the probe found none: say why at once. A socket that is still
    // connecting (the first seconds after start, a slow TLS handshake) is not a failure: the
    // request is queued until it opens (backend_social_send) and AUTH_TIMEOUT_SECONDS reports
    // a socket that never does.
    if (name && (!strcmp(name, "registerAcc") || !strcmp(name, "loginAcc")) && (!backend_enabled() || backend_available() == 0)) {
        if (backend_enabled()) backend_probe_start(); // maybe the backend came up since: ask again
        auth_unreachable();
        return 1;
    }
    // krunker.io is the active backend: a krunk.io login form never sends its fields to kn_brain
    // (v10's login modal signs in to krunker.io itself, v10_login.c)
    if (name && (!strcmp(name, "registerAcc") || !strcmp(name, "loginAcc")) && account_backend() == ACCOUNT_BACKEND_KRUNKER) return 1;
    if (name && !strcmp(name, "logoutAcc") && account_backend() == ACCOUNT_BACKEND_KRUNKER) { // also without a brain
        account_ui_logout();
        return 1;
    }
    { // Privacy & Age (kid_settings.c): the krunker.io account's k-ID switches, Ask a Parent, Show Code, the parent dialog
        const int kid = kid_settings_handle(name, args, argc);
        if (kid >= 0) return kid;
    }
    if (name && !g_logged_in && reset_handle(name)) return 1; // KrunkNative forgot password (works without a brain: says why)
    if (!name || (!backend_enabled() && !g_scenario)) return -1; // g_scenario: --render-scenario's stand-in account
    if (battlepass_ui_handle(name, ev, args, argc) >= 0) return 1; // before the Store: creatorCodeAction(.., 3)
    if (store_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (clan_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (ranked_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (turf_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (!strcmp(name, "registerAcc") || !strcmp(name, "loginAcc")) {
        char user[256], pass[128], mail[256];
        read_input("accName", user, sizeof(user));
        read_input("accPass", pass, sizeof(pass));
        read_input("accEmail", mail, sizeof(mail));
        // a krunker.io login or name typed on this card (account_split.h): the Box Office card never sends it to kn_brain
        const int split_refused = account_split_refuses(user[0] ? user : mail) || account_split_refuses(mail);
        if (!strcmp(name, "registerAcc")) {
            // Krunker v10 registers by email ("Please enter your email to register."), and the
            // forgot-password flow mails the account's address: a new account needs one
            const char *m = mail;
            while (*m == ' ') m++;
            if (!*m) {
                show_acc_resp(I18N("generic.auth.emailRequiredForRegister", "Please enter your email to register."));
                memset(pass, 0, sizeof(pass));
                return 1;
            }
            if (split_refused) {
                show_acc_resp(I18N("kn.account.splitRegister", "A krunker.io account has this name or email. Choose another for Box Office."));
                memset(pass, 0, sizeof(pass));
                return 1;
            }
            send_account_request_email(0, user, pass, NULL, mail); // [name, pass, referralCode, email]
        } else if (names_other_account(user, mail)) { // the session stays: the card says whose it is, nothing is sent
            show_other_signed_in(user[0] ? user : mail);
            memset(pass, 0, sizeof(pass));
            return 1;
        } else if (split_refused) {
            show_acc_resp(I18N("kn.account.splitKrunkerIo", "That is a krunker.io account. Sign in to it on an official krunker.io game."));
            memset(pass, 0, sizeof(pass));
            return 1;
        } else {
            // the login field takes a username or an email (v10's "Email" / "Username" field: the
            // brain signs in to the account holding the address)
            send_account_request(1, user[0] ? user : mail, pass, NULL);
        }
        memset(pass, 0, sizeof(pass));
        g_auth_sent_at = glfwGetTime();
        return 1;
    }
    if (!strcmp(name, "logoutAcc")) {
        account_ui_logout();
        return 1;
    }
    if (!strcmp(name, "logoutSessions")) { // io.send("las")
        g_logout_sessions = 1;
        if (ev && ev->target) html_dom_set_display(ev->target, "none");
        backend_social_send_text("[\"las\"]");
        return 1;
    }
    if (!strcmp(name, "knPfpPick")) { // "Change Picture": pick a PNG / JPEG / WebP, upload to kn_brain (avatar_ui.c)
        avatar_ui_pick();
        return 1;
    }
    if (progress_handle(name, args, argc) >= 0) return 1; // WP-E part 2c: badges, windows[32]
    if (!strcmp(name, "windows.4.switchStat")) { // statTabB<n> shown, statTab<n> active
        const int t = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        for (int i = 0; i < STAT_TAB_COUNT; i++) { // + KrunkNative's Medals tab (WP-E part 2c)
            char sel[24], id[16];
            snprintf(sel, sizeof(sel), ".statTabB%d", i);
            snprintf(id, sizeof(id), "statTab%d", i);
            HtmlElem b = html_dom_query(0, sel), tab = html_dom_by_id(id);
            if (b) html_dom_set_class(b, "hide", i != t);
            if (tab) html_dom_set_class(tab, "bigMenTabA", i == t);
        }
        return 1;
    }
    if (!strcmp(name, "windows.4.switchTab")) {
        g_tab_index = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        menu_show_window(5, 1); // updateWindow(5)
        return 1;
    }
    if (!strcmp(name, "windows.4.collapseCon")) { // the Connections tab's Discord / Twitch headers
        HtmlElem hd = argc > 0 && args[0].kind == HTML_ARG_THIS ? (HtmlElem) args[0].num : 0;
        if (!hd && argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) hd = html_dom_by_id(args[0].str); // a scenario's windowAction: the header's id
        collapse_con(hd ? hd : ev ? ev->target : 0);
        return 1;
    }
    if (!strcmp(name, "togglePromoEmails")) { // window.togglePromoEmails(on): if (account) setPreference(!!on, "settings")
        const int on = argc > 0 && args[0].kind == HTML_ARG_THIS_CHECKED ? ev && ev->checked : argc > 0 && args[0].num != 0.0;
        if (g_logged_in) promo_emails_set(on);
        return 1;
    }
    if (!strcmp(name, "linkTwitchPop") || !strcmp(name, "unlinkTwitchPop")) {
        // v10 asks "Are you sure you want to link a twitch account?" (connectTwitchAcc, deob-10.0.3.js:261960-262004).
        // The OAuth page comes from kn_brain (account_life.c); unlink keeps the permanent pair, as v10 warns.
        if (!g_logged_in) return 1;
        if (name[1] == 'i') // linkTwitchPop
            ui_confirm("Are you sure you want to link a twitch account?", twitch_link_confirmed, NULL);
        else
            ui_confirm("Unlinking does not allow you to link a seperate twitch account. Unlink?", twitch_unlink_confirmed, NULL);
        return 1;
    }
    if (!strcmp(name, "claimDrops")) { // window.windows[4].claimDrops: Box Office has no Twitch drop campaigns
        if (g_logged_in) backend_social_send_text("[\"getTwitchDrops\"]");
        return 1;
    }
    if (!strcmp(name, "requestDeletion")) { // window.requestDeletion(flag): v10's rpc("rqd", flag)
        const int flag = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num != 0 : 1;
        if (!g_logged_in) return 1;
        if (flag)
            ui_confirm(I18N("kn.account.delete.confirm", "Request the deletion of this account and all associated data? You will be logged out."),
                       deletion_confirmed, (void *) (intptr_t) 1);
        else
            deletion_confirmed(1, NULL, (void *) (intptr_t) 0);
        return 1;
    }
    if (!strcmp(name, "claimReward")) {
        // The Free KR card (window.claimReward, deob-10.0.5.js:129955; v10 calls it but no longer defines it, so this is
        // v7.1.4's, index.js:3168-3192): "sfkr", an ad (onShow "lfkr", onError "fkrf"), then the Free KR wheel
        // (prizeWheel(4)) whose spin sends "fkr" (AD_FINISHED, :222644). The ad is KrunkNative's placeholder (kn_ad.h).
        if (account_reward_ms() != 0.0) return 1; // no account, or the timer has not run out (vLN01713 > 0)
        ui_loadout_clear_pops();
        free_kr_ad_frame("sfkr");
        const KnAdRequest ad = {.on_show = free_kr_ad_shown, .on_complete = free_kr_ad_done, .on_error = free_kr_ad_error};
        kn_ad_show(&ad);
        return 1;
    }
    if (!strcmp(name, "windows.29.switchTab")) {
        const int tab = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        const int force = argc > 1;
        if (g_mail_view != tab || force) {
            g_mail_view = tab >= 0 && tab < 5 ? tab : 3;
            g_mail_tab = g_mail_view == 4;
            cJSON_Delete(g_mail_rows);
            g_mail_rows = NULL;
            g_mail_loaded = 0;
            g_mail_failed = 0;
            menu_show_window(30, 1);
        }
        return 1;
    }
    if (!strcmp(name, "windows.29.readMail")) { // toggle the text; "Loading..." + io.send("rml", id)
        HtmlElem obj = ev ? ev->target : 0;
        const HtmlElem text = obj ? html_dom_query(obj, ".mailText") : 0;
        if (!text) return 1;
        char cur[256] = "";
        html_dom_get_text(text, cur, sizeof(cur));
        if (cur[0] && strcmp(cur, read_failed_text())) {
            html_dom_set_html(text, "");
        } else { // closed, or the failed read asked again
            const int id = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : 0;
            if (g_read_id && g_read_id != id) read_failed(g_read_id); // another read is still in flight: its row says so
            read_pending_clear();
            html_dom_set_html(text, "Loading...");
            char frame[48];
            snprintf(frame, sizeof(frame), "[\"rml\",%d]", id);
            g_read_id = id;
            g_read_timer = html_timer_add(MAIL_READ_WAIT_MS, read_expired, NULL);
            if (!backend_social_send_text(frame)) { // no socket: no answer can come
                read_pending_clear();
                read_failed(id);
            }
        }
        return 1;
    }
    if (!strcmp(name, "windows.29.readAll")) {
        backend_social_send_text("[\"raml\"]");
        g_mail_tab = 1;
        g_mail_view = 4;
        cJSON_Delete(g_mail_rows);
        g_mail_rows = NULL;
        g_mail_loaded = 0;
        g_mail_failed = 0;
        mail_count(0);
        menu_show_window(30, 1);
        return 1;
    }
    if (!strcmp(name, "onInput2FA")) {
        twofa_on_input(ev ? ev->target : 0);
        return 1;
    }
    if (!strcmp(name, "onKeyDown2FA")) return twofa_on_keydown(ev ? ev->target : 0, ev ? ev->key : 0);
    if (!strcmp(name, "sub2FA")) {
        twofa_submit();
        return 1;
    }
    if (!strcmp(name, "act2FA") && g_logged_in) { // io.send("a2FA")
        backend_social_send_text("[\"a2FA\"]");
        return 1;
    }
    if (!strcmp(name, "dact2FA") && g_logged_in) { // io.send("d2FA")
        backend_social_send_text("[\"d2FA\"]");
        return 1;
    }
    if (!strcmp(name, "changeEmail") && g_logged_in) { // KrunkNative: ["cemail", email]
        char addr[256];
        read_input("accEmailNew", addr, sizeof(addr));
        if (!addr[0]) {
            email_res("Enter an Email", 1);
            return 1;
        }
        email_res(ACC_STR_WAIT, 0);
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("cemail"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(addr));
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    if (!strcmp(name, "resendEmail") && g_logged_in) { // KrunkNative: ["emailv"]
        email_res(ACC_STR_WAIT, 0);
        backend_social_send_text("[\"emailv\"]");
        return 1;
    }
    if (!strcmp(name, "uploadSettings") || !strcmp(name, "fetchSettings")) {
        if (!g_logged_in) {
            popup_response(0, I18N("account.login.required", "Must be logged in"), NULL);
            return 1;
        }
        if (!strcmp(name, "fetchSettings")) {
            backend_social_send_text("[\"getS\"]");
            return 1;
        }
        popup_response(2, NULL, "Loading..."); // dP("Loading...")
        char *text = ui_settings_export_text();
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("uploadS"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(text ? text : "{}"));
        backend_social_send(frame);
        cJSON_Delete(frame);
        free(text);
        return 1;
    }
    if (!strcmp(name, "redeemVoucher")) { // io.send("vr", code); v10 sends nothing for a blank box
        char code[64];
        read_input("redVouch", code, sizeof(code));
        if (!code[0]) return 1;
        HtmlElem res = html_dom_by_id("vouchRes"); // v10: "Loading..." until the answer
        if (res) {
            html_dom_set_text(res, "Loading...");
            html_dom_set_display(res, "block");
        }
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("vr"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(code));
        html_timer_cancel(g_vouch_timer);
        g_vouch_timer = html_timer_add(VOUCHER_WAIT_MS, vouch_expired, NULL);
        g_vouch_pending = 1;
        if (!backend_social_send(frame)) { // no socket: no answer can come
            html_timer_cancel(g_vouch_timer);
            g_vouch_timer = 0;
            g_vouch_pending = 0;
            if (res) html_dom_set_text(res, I18N("kn.vouch.noAnswer", "Couldn't reach the server. Try the code again."));
        }
        cJSON_Delete(frame);
        return 1;
    }
    if (!strcmp(name, "selectFlag")) { // if (acc) io.send("flg", parseInt(value))
        const char *v = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
        const int flag = v ? atoi(v) : argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        if (g_logged_in) {
            char frame[48];
            snprintf(frame, sizeof(frame), "[\"flg\",%d]", flag);
            backend_social_send_text(frame);
        }
        return 1;
    }
    if (!strcmp(name, "changePass")) { // io.send("upp", old, new, repeat)
        char old_pass[128], new_pass[128], rep[128];
        HtmlElem res = html_dom_by_id("newPRes");
        if (res) html_dom_set_html(res, "Please wait...");
        read_input("oldPass", old_pass, sizeof(old_pass));
        read_input("newPass", new_pass, sizeof(new_pass));
        read_input("repNPass", rep, sizeof(rep));
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("upp"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(old_pass));
        cJSON_AddItemToArray(frame, cJSON_CreateString(new_pass));
        cJSON_AddItemToArray(frame, cJSON_CreateString(rep));
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    if (!strcmp(name, "changeName")) { // dP("Please Wait..."); io.send("nme", displayN.value)
        char alias[64];
        popup_response(2, NULL, "Please Wait...");
        read_input("displayN", alias, sizeof(alias));
        if (!g_logged_in || !g_acc.premium || !alias[0]) {
            popup_response(0, "Invalid name", NULL);
            return 1;
        }
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("nme"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(alias));
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    return -1;
}

// ------------------------------------------------------------------ windows

static void stat_row(StrBuf *sb, const char *label, const char *value) {
    sb_append(sb, "<div class='settName'>");
    sb_append(sb, label);
    sb_append(sb, "<span class='floatR'>");
    sb_append(sb, value);
    sb_append(sb, "</span></div>");
}

static void stat_row_num(StrBuf *sb, const char *label, const double v) {
    char text[32];
    snprintf(text, sizeof(text), "%.0f", v);
    stat_row(sb, label, text);
}

static void stat_row_fixed(StrBuf *sb, const char *label, const double v) {
    char text[32];
    snprintf(text, sizeof(text), "%.2f", v);
    stat_row(sb, label, text);
}

// UTILS.xpBarText(acc)
static void xp_bar_text(char *out, const size_t n) {
    const double m = g_acc.level > 1 ? pow(g_acc.level / RANK_VAR, 2) : 0.0;
    snprintf(out, n, "%d / %d", (int) (g_acc.score - m), (int) (pow((g_acc.level + 1) / RANK_VAR, 2) - m));
}

// UTILS.commaFloat(v.toFixed(decimals)): thousands separators on the integer part, the
// toFixed fraction kept as is (v7 passes the toFixed string: SPK "12,500,000.00", KDR "0.0000")
static void comma_float(char *out, const size_t n, const double v, const int decimals) {
    char fixed[64], whole[48];
    snprintf(fixed, sizeof(fixed), "%.*f", decimals > 0 ? decimals : 0, v);
    char *dot = strchr(fixed, '.');
    if (dot) *dot = 0;
    comma_number(whole, sizeof(whole), strtod(fixed, NULL));
    if (fixed[0] == '-' && whole[0] != '-') { // -0.5 -> "-0"
        memmove(whole + 1, whole, strlen(whole) + 1);
        whole[0] = '-';
    }
    snprintf(out, n, "%s%s%s", whole, dot ? "." : "", dot ? dot + 1 : "");
}

static void pst(StrBuf *sb, const char *label, const char *value) {
    sb_appendf(sb, "<div class='pSt'>%s<strong>%s</strong></div>", label, value);
}

static void pst_num(StrBuf *sb, const char *label, const double v, const int decimals) {
    char t[48];
    comma_float(t, sizeof(t), v, decimals);
    pst(sb, label, t);
}

// windows[4]'s "Your Stats" (the XP bar and the stat tabs) for an account: the Box Office one
// (g_acc) or the krunker.io one (its "a" / "ua" data from the official game server)
static double stat_in(const AccountInfo *acc, const char *key) {
    const cJSON *v = acc->stats ? cJSON_GetObjectItem(acc->stats, key) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

static void xp_text(const AccountInfo *acc, char *out, const size_t n) {
    const double m = acc->level > 1 ? pow(acc->level / RANK_VAR, 2) : 0.0;
    snprintf(out, n, "%d / %d", (int) fmax(acc->score - m, 0), (int) (pow((acc->level + 1) / RANK_VAR, 2) - m));
}

// a count as JS prints a number ("12", "1.5")
static void js_number(char *out, const size_t n, const double v) {
    if (v == floor(v) && fabs(v) < 1e15) snprintf(out, n, "%.0f", v);
    else snprintf(out, n, "%.15g", v);
}

// a raid entry's number field; a map_id may come as text
static double raid_num(const cJSON *entry, const char *key) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(entry, key);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) ? atof(v->valuestring) : 0.0;
}

// acc.raidStats: the krunker.io account's comes in setData's slot 52 ("ua", kio_set_data). kn_brain keeps no
// raid clears (leaderboards.c), so a Box Office account has v10's empty list (a scenario's account.raidStats).
static const cJSON *raid_stats_of(const AccountInfo *acc) {
    const cJSON *list = acc == &g_kio_acc ? cJSON_GetObjectItemCaseSensitive(g_kio.obj, "raidStats") : g_raid_stats;
    return cJSON_IsArray(list) ? list : NULL;
}

// statTabB4, Raids (deob-10.0.3.js:247596-247634): Totals, AI Kills, Boss Kills and Deaths over every entry of
// raidStats [{map_id, completions, attempts, ai_kills, boss_kills, deaths, best_time}], then one card per Raid map
// (Tortuga 18, Laboratory 29, Soul Sanctum 31, Khepri 35, Bastion 43; the Raid host's allowedDat). No entry: zeros
// and a best time of --:--.
static void raids_html(StrBuf *sb, const cJSON *list) {
    static const struct {
        int map;
        const char *name;
    } maps[] = {{18, "Tortuga"}, {29, "Laboratory"}, {31, "Soul Sanctum"}, {35, "Khepri"}, {43, "Bastion"}};
    double done = 0, tries = 0, ai = 0, boss = 0, deaths = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, list) {
        done += raid_num(e, "completions");
        tries += raid_num(e, "attempts");
        ai += raid_num(e, "ai_kills");
        boss += raid_num(e, "boss_kills");
        deaths += raid_num(e, "deaths");
    }
    char c[32], t[32], line[96];
    js_number(c, sizeof(c), done);
    js_number(t, sizeof(t), tries);
    snprintf(line, sizeof(line), "%s/%s (%.0f%%)", c, t, tries > 0 ? floor(done / tries * 100 + 0.5) : 0.0); // Math.round
    pst(sb, "Totals", line);
    pst_num(sb, "AI Kills", ai, 0);
    pst_num(sb, "Boss Kills", boss, 0);
    pst_num(sb, "Deaths", deaths, 0);
    sb_append(sb, "<div style='height:10px'></div>");
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        const cJSON *r = NULL;
        cJSON_ArrayForEach(e, list) if (raid_num(e, "map_id") == maps[m].map) { // map_id == key (a loose compare)
            r = e;
            break;
        }
        const double rd = raid_num(r, "completions"), rt = raid_num(r, "attempts");
        char best[24] = "--:--";
        const cJSON *bt = cJSON_GetObjectItemCaseSensitive(r, "best_time");
        if (r && bt && !cJSON_IsNull(bt)) { // floor(ms / 60000) + ":" + ("0" + floor(ms % 60000 / 1000)).slice(-2)
            const double ms = raid_num(r, "best_time");
            snprintf(best, sizeof(best), "%.0f:%02d", floor(ms / 60000), (int) floor(fmod(ms, 60000) / 1000));
        }
        js_number(c, sizeof(c), rd);
        js_number(t, sizeof(t), rt);
        sb_appendf(sb, "<div class='pSt' style='text-align:center;padding:10px;height:100%%;width: 180px;'><div class='pStTitle'>%s</div>"
                       "<div class='pSt' style='margin:2px 0;'>Completions<strong>%s/%s (%.0f%%)</strong></div>"
                       "<div class='pSt' style='margin:2px 0;'>Best Time<strong>%s</strong></div>",
                   maps[m].name, c, t, rt > 0 ? floor(rd / rt * 100 + 0.5) : 0.0, best);
        const char *const keys[][2] = {{"ai_kills", "AI Kills"}, {"boss_kills", "Boss Kills"}, {"deaths", "Deaths"}};
        for (int k = 0; k < 3; k++) {
            comma_float(line, sizeof(line), raid_num(r, keys[k][0]), 0);
            sb_appendf(sb, "<div class='pSt' style='margin:2px 0;'>%s<strong>%s</strong></div>", keys[k][1], line);
        }
        sb_append(sb, "</div>");
    }
}

static void stats_html(StrBuf *sb, const AccountInfo *acc, const int kio) {
    // statTabs (deob-10.0.3.js:21520) + KrunkNative's Medals (the account's medal counts, kn_brain "mdl", WP-E part 2c):
    // a Box Office account's only, krunker.io keeps no medal counts
    static const char *const stat_tabs[STAT_TAB_COUNT] = {"General Stats", "Killstreaks", "Other Stats", "Class Stats", "Raids", "Medals"};
    const int tab_count = kio ? STAT_TAB_MEDALS : STAT_TAB_COUNT;
    char a[64];
    sb_append(sb, "<div class='setHed' style='margin-top:10px;margin-bottom:15px;'>Your Stats</div>");
    xp_text(acc, a, sizeof(a));
    sb_appendf(sb, "<div class='xpBar' style='height:31px;line-height:34px;font-size:28px;'><div class='xpBarB' style='width:%.6g%%'></div><span class='xpBarV'>%s</span></div>", acc->level_prog, a);
    sb_append(sb, "<div id='statHolder'><div style='height:15px;'></div><div class='bigMenTabs' style='margin-bottom: 5px;table-layout: fixed;'>");
    for (int i = 0; i < tab_count; i++) {
        sb_appendf(sb, "<div id='statTab%d' class='bigMenTab%s' onclick='playSelect(0.1);windows[4].switchStat(%d)' style='padding-left: 5px; padding-right: 5px; padding-top: 10px; padding-bottom: 5px; line-height: 35px;font-size: 16px;'onmouseenter='playTick();'>%s</div>",
                   i, i == 0 ? " bigMenTabA" : "", i, stat_tabs[i]);
    }
    sb_append(sb, "</div><div class='statTabB0'>");
    const double losses = acc->games - acc->wins > 0 ? acc->games - acc->wins : 0;
    const double shots = stat_in(acc, "s"), hits = fmin(stat_in(acc, "s"), stat_in(acc, "h"));
    snprintf(a, sizeof(a), "%d", acc->level);
    pst(sb, "LVL", a);
    pst_num(sb, "KR", acc->funds, 0);
    pst_num(sb, "Junk", acc->junk, 0); // acc.junk (the krunker.io account's player_jnk; Box Office accounts have none)
    pst(sb, "Inventory", " ~0");
    pst_num(sb, I18N("generic.score", "Score"), acc->score, 0);
    pst_num(sb, "SPK", acc->score / fmax(acc->kills, 1), 2);
    pst_num(sb, I18N("generic.kills", "Kills"), acc->kills, 0);
    pst_num(sb, I18N("generic.deaths", "Deaths"), acc->deaths, 0);
    pst_num(sb, I18N("generic.kdr", "KDR"), acc->kills / fmax(acc->deaths, 1), 4);
    pst_num(sb, "KPG", acc->kills / fmax(acc->games, 1), 4);
    pst_num(sb, I18N("generic.games", "Games"), acc->games, 0);
    snprintf(a, sizeof(a), "%g", round(acc->wins / fmax(losses, 1) * 1000) / 1000);
    pst(sb, "W/L", a);
    pst_num(sb, I18N("generic.wins", "Wins"), acc->wins, 0);
    pst_num(sb, I18N("generic.lose", "Losses"), losses, 0);
    pst_num(sb, I18N("stats.assists", "Assists"), stat_in(acc, "ast"), 0);
    pst_num(sb, I18N("stats.melee", "Melee"), stat_in(acc, "mk"), 0);
    pst_num(sb, I18N("stats.fistkills", "Beatdowns"), stat_in(acc, "fk"), 0);
    pst_num(sb, I18N("stats.thrownkills", "Bullseyes"), stat_in(acc, "tmk"), 0);
    pst_num(sb, I18N("stats.headshots", "Headshots"), stat_in(acc, "hs"), 0);
    pst_num(sb, I18N("stats.legshots", "Legshots"), stat_in(acc, "ls"), 0);
    pst_num(sb, I18N("stats.wallbangs", "Wallbangs"), stat_in(acc, "wb"), 0);
    pst_num(sb, I18N("stats.shots", "Shots"), shots, 0);
    pst_num(sb, I18N("stats.hits", "Hits"), hits, 0);
    pst_num(sb, I18N("stats.misses", "Misses"), shots - hits, 0);
    snprintf(a, sizeof(a), "%g%%", shots > 0 ? round(hits / shots * 100 * 100) / 100 : 0.0);
    pst(sb, I18N("generic.accuracy", "Accuracy"), a);
    readable_time(a, sizeof(a), acc->time_played);
    pst(sb, I18N("generic.timep", "Time Played"), a);
    sb_append(sb, "</div><div class='statTabB1 hide'>");
    pst_num(sb, I18N("stats.nukes", "Nukes"), stat_in(acc, "n"), 0);
    pst_num(sb, I18N("stats.airdrops", "Airdrops"), stat_in(acc, "ad"), 0);
    pst_num(sb, I18N("stats.airdrops.stolen", "Airdrops Stolen"), stat_in(acc, "sad"), 0);
    pst_num(sb, I18N("stats.slimers", "Slimers"), stat_in(acc, "sl"), 0);
    pst_num(sb, I18N("stats.juggernauts", "Juggernauts"), stat_in(acc, "jg"), 0);
    sb_appendf(sb, "<div class='pSt' style='font-size: 12px;padding-left: 0px;padding-right: 0px;width: 174px;'>%s<strong>%.0f</strong></div>",
               I18N("stats.juggernauts.killed", "Juggernauts Killed"), stat_in(acc, "kjg"));
    pst_num(sb, I18N("stats.warmachines", "War Machines"), stat_in(acc, "wm"), 0);
    sb_append(sb, "</div><div class='statTabB2 hide'>");
    pst_num(sb, I18N("stats.crouches", "Crouches"), stat_in(acc, "crc"), 0);
    pst_num(sb, I18N("stats.sprays", "Sprays Placed"), stat_in(acc, "spry"), 0);
    char kpd[32];
    snprintf(kpd, sizeof(kpd), "%d/%d", acc->job_rating_pos, acc->job_rating); // jobRatingPos / jobRating
    pst(sb, I18N("stats.kpd", "Helpful Reports"), kpd);
    sb_append(sb, "</div><div class='statTabB3 hide'>");
    const MenuUi *ui = menu_ui();
    for (int i = 0; i < ui->class_count; i++) {
        const MenuClass *c = &ui->classes[i];
        char key[16];
        snprintf(key, sizeof(key), "c%d", c->index);
        const double xp = stat_in(acc, key), lv = RANK_VAR * sqrt(xp);
        int level = (int) floor(lv);
        const int pct = (int) lround((lv - level) * 100.0);
        const double lo = level > 1 ? pow(level / RANK_VAR, 2) : 0.0, hi = pow((level + 1) / RANK_VAR, 2);
        level = level + 1 > 1 ? level + 1 : 1;
        sb_appendf(sb, "<div class='clsStatCard'><img class='clsStatImg' src='%s'/><div class='clsStatName'>", c->icon);
        sb_append_esc(sb, c->name ? c->name : "");
        sb_append(sb, "</div><div class='clsStatWeap'>");
        sb_append_esc(sb, c->weapon ? c->weapon : "");
        sb_appendf(sb, "</div><div class='clsStatXPBar'><div class='clsStatXPBarC' style='width:%d%%'></div><div class='clsStatXPBarV'>%d / %d</div></div><div class='clsStatLvl'>Lvl %d</div></div>",
                   pct, (int) (xp - lo), (int) (hi - lo), level);
    }
    sb_append(sb, "</div><div class='statTabB4 hide'>");
    raids_html(sb, raid_stats_of(acc));
    sb_append(sb, "</div>");
    if (!kio) { // statTabB5: KrunkNative's medal counts (kn_brain "mdl"): no such data for a krunker.io account
        sb_append(sb, "<div class='statTabB5 hide'>");
        medal_tab_html(sb);
        if (!g_medal_counts) backend_social_send_text("[\"mdl\"]");
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

// The Connections tab's Twitch section (deob-10.0.3.js:247697-247750). The link runs through kn_brain
// (account_life.c): twitchAuth returns the OAuth page to open in the browser, twitchVerifyResponse the linked
// account. "Drops Earned" lists the owned items with `drop`
// (iììiíîï(account, true): the inventory, every store item for a developer; v10's extra free / creator /
// streamer items are not drops), by rarity, then season, then index, all descending.
typedef struct {
    int ind, rarity, seas;
} DropItem;

static int drop_cmp(const void *pa, const void *pb) {
    const DropItem *a = pa, *b = pb;
    if (a->rarity != b->rarity) return b->rarity - a->rarity;
    if (a->seas != b->seas) return b->seas - a->seas;
    return b->ind - a->ind;
}

static void drops_add(DropItem **items, int *n, int *cap, const int ind) {
    const ViewerSkin *s = viewer_skin(ind);
    if (!s || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ui_loadout_skin_json(ind), "drop"))) return;
    if (*n == *cap) {
        const int grown = *cap ? *cap * 2 : 16;
        DropItem *more = realloc(*items, (size_t) grown * sizeof(**items));
        if (!more) return;
        *items = more;
        *cap = grown;
    }
    (*items)[(*n)++] = (DropItem){ind, s->rarity, s->seas ? s->seas : 1};
}

static void twitch_html(StrBuf *sb) {
    DropItem *items = NULL;
    int n = 0, cap = 0;
    if (g_acc.is_dev) {
        for (int i = 0; i < g_viewer_skin_count; i++) drops_add(&items, &n, &cap, i);
    } else {
        const cJSON *e, *inv = g_acc.data ? cJSON_GetArrayItem(g_acc.data, 10) : NULL; // acc.skins [{ind, cnt}]
        cJSON_ArrayForEach(e, inv) {
            const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind");
            if (cJSON_IsNumber(ind)) drops_add(&items, &n, &cap, ind->valueint);
        }
    }
    if (n > 1) qsort(items, (size_t) n, sizeof(*items), drop_cmp);
    int frames[16], frame_n = 0; // the sprays' sprite-sheet lengths: one @keyframes sprayAnmD<frames> each
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = viewer_skin(items[i].ind);
        if (s->type != 4 || s->frames <= 0) continue;
        int k = 0;
        while (k < frame_n && frames[k] != s->frames) k++;
        if (k == frame_n && frame_n < (int) (sizeof(frames) / sizeof(frames[0]))) frames[frame_n++] = s->frames;
    }
    if (frame_n) { // prepended to the section, as v10 does
        sb_append(sb, "<style>");
        for (int k = 0; k < frame_n; k++) sb_appendf(sb, "@keyframes sprayAnmD%d {100%%{background-position:-%dpx}}", frames[k], frames[k] * 68);
        sb_append(sb, "</style>");
    }
    sb_append(sb, "<div class='setHed' id='setHed_twitch' style='margin-top:0px'onclick='window.windows[4].collapseCon(this)'><span class='material-icons plusOrMinus3'>keyboard_arrow_right</span> "
                  "<img src='./img/connections/twitch.png' style='height: 40px;'></div>");
    sb_append(sb, "<div class='setBodH' id='setBod_twitch' style='display:none'><div style=\"margin-bottom: 20px;margin-top:10px;text-align: center;color: var(--yellow);font-size: 19px;\">\n\t\t"
                  "Twitch linking creates a permanent connection to this Krunker account regardless of unlinking</div><div class='settName'>Twitch Account (");
    const char *twitch = g_twitch_name[0] ? g_twitch_name : g_twitch_id; // acc.twitch || acc.twitchId || "none"
    sb_append_esc(sb, twitch[0] ? twitch : "none");
    // v10 (deob-10.0.3.js:247700): Claim when linked, Link / Unlink by acc.twitchId
    if (g_twitch_id[0])
        sb_append(sb, ")<div class='lnkTw' style='background-color:#4144a5;' onclick='claimDrops();'>Claim</div>"
                       "<div class='lnkTw' onclick='unlinkTwitchPop();'>Unlink</div></div>");
    else
        sb_append(sb, ")<div class='lnkTw' onclick='linkTwitchPop();'>Link</div></div>");
    sb_append(sb, "<div style='margin-top:10px;'>Link <a href='https://www.twitch.tv/directory/game/Krunker' target='_blank' style='color:#6441a5'>Twitch</a> to earn Drops & Items</div>");
    sb_append(sb, "<div id='skinList' style='text-align: center;'>");
    if (n) sb_append(sb, "<div class='setHed' style='margin-bottom:20px;'>Drops Earned</div>");
    for (int i = 0; i < n; i++) {
        const ViewerSkin *s = viewer_skin(items[i].ind);
        char path[256], url[512];
        viewer_skin_preview_path(s, path, sizeof(path));
        html_dom_asset_url(path, url, sizeof(url));
        const char *color = s->rarity >= 0 && s->rarity < g_viewer_rarity_count ? g_viewer_rarities[s->rarity].color : "#B0B0B0";
        sb_appendf(sb, "<div class='dItemHolder' style='border: 2px solid %s;' onclick='openSales(%d)'>", color, items[i].ind);
        if (s->type == 4) {
            sb_appendf(sb, "<div class='dItemSprayImg' style='background: url(&quot;%s&quot;);", url);
            if (s->frames > 0) sb_appendf(sb, "animation: sprayAnmD%d %.15gs steps(%d) infinite;", s->frames, 1 + s->frame_t / 1000.0, s->frames);
            sb_append(sb, "'></div>");
        } else {
            sb_appendf(sb, "<img draggable='false' class='dItemImg' loading='lazy' src='%s'>", url);
        }
        sb_append(sb, "<div class='tItemN'>");
        sb_append_esc(sb, s->name ? s->name : "");
        sb_append(sb, "</div></div>");
    }
    sb_append(sb, "</div></div>");
    free(items);
}

// The Connections tab (deob-10.0.3.js:247686-247752): the Discord section, then Twitch's. v10 mounts its Discord
// link component (svelte-17wd35, deob-10.0.3.js:110353-110561) in #discordLinkMount; it asks discord.getStatus()
// and kn_brain's GET /discord/status answers {linked: false} with no `configured` (v10_auth.c: Box Office has no
// Discord link), so the component's state is v10's not-configured one: the info line, "Discord Account (none)"
// with no Link button, and "Discord linking is currently unavailable.". It is drawn in that state at once.
static void connections_html(StrBuf *sb) {
    sb_append(sb, "<div class='setHed' id='setHed_discord' style='margin-top:0px'onclick='window.windows[4].collapseCon(this)'><span class='material-icons plusOrMinus3'>keyboard_arrow_right</span> "
                  "<img src='./img/social/discord.png' style='height: 40px;vertical-align: middle;'> Discord</div><div class='setBodH' id='setBod_discord' style='display:none'><div id='discordLinkMount'>");
    sb_append(sb, "<div class=\"discordInfo svelte-17wd35\">");
    sb_append_esc(sb, I18N("discord.info", "Link Discord to get your Krunker roles in the Krunker Discord server and join Discord events."));
    sb_append(sb, "</div> <div class=\"settName discordRow\"> ");
    sb_append_esc(sb, I18N("discord.account", "Discord Account"));
    sb_append(sb, " (");
    sb_append_esc(sb, I18N("discord.none", "none"));
    sb_append(sb, ") </div> <div class=\"discordMsg svelte-17wd35\">");
    sb_append_esc(sb, I18N("discord.result.unavailable", "Discord linking is currently unavailable."));
    sb_append(sb, "</div></div></div>");
    twitch_html(sb);
}

// windows[4].gen() for a signed-in Box Office account (deob-10.0.3.js:247488-247763; a guest gets the login modal,
// v10_login.c): the profileTabs (home = Your Account + the stat tabs, Friends, Settings, Connections) and the
// logout links.
static char *gen_account(void) {
    static const char *const tabs[PROFILE_TAB_COUNT] = {"<span class='material-icons' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>",
                                                        "Friends", "Settings", "Connections"}; // profileTabs (deob-10.0.3.js:21519)
    StrBuf sb = {0};
    sb_append(&sb, "<div style='height:15px;'></div><div class='bigMenTabs'>");
    for (int i = 0; i < PROFILE_TAB_COUNT; i++)
        sb_appendf(&sb, "<div class='bigMenTab%s' style='font-size:16px' onmouseenter='playTick(0.1);' onclick='playSelect();windows[4].switchTab(%d)'>%s</div>", g_tab_index == i ? " bigMenTabA" : "", i, tabs[i]);
    sb_append(&sb, "</div>");
    if (g_tab_index == PROFILE_TAB_HOME) {
        sb_append(&sb, "<div class='setHed' style='margin-top:10px;margin-bottom:20px;'>Your Account</div>");
        sb_append(&sb, "<div id='profilePicM' class='blurred'><img style='width: inherit;height: inherit;border-radius: inherit;' loading='lazy' src='textures/classes/icon_0.png'><a class='pfpAction' onmouseenter='playTick(0.1)' onclick='knPfpPick();'>");
        sb_append(&sb, I18N("windows.profile.picture", "Change Picture"));
        // The upload status line (avatar_ui.c set_status) is hidden until it has text: an empty settName drew
        // a rule under the picture and, as a block, pushed the name box below it (v7.1.4 / v10 have neither).
        sb_append(&sb, "</a></div><div id='pfpStatus' style='display:none;width:181px;font-size:13px;margin-top:6px;color:rgba(255,255,255,0.6)'></div>");
        sb_append(&sb, "<div class='setBodH' style='display:inline-block;padding-top:10px;vertical-align:top;width:1004px;margin-left:15px;margin-top:0px;'><div class='settName' style='width:100%;margin-top:0px'>");
        sb_append(&sb, I18N("generic.name", "Name"));
        if (player_names_hidden()) sb_append(&sb, "<a class='floatR'>"); // no profile link while names are hidden (the real account's)
        else { // openPlayerProfile("name"): the player's profile view (deob-10.0.3.js:247528)
            sb_append(&sb, "<a href='javascript:void(0)' onclick='openPlayerProfile(\"");
            for (const char *p = g_acc.name; *p; p++) { // the name as a JS string inside the attribute
                char c[3] = {'\\', *p, 0};
                sb_append_esc(&sb, *p == '\'' || *p == '"' || *p == '\\' ? c : c + 1);
            }
            sb_append(&sb, "\")' class='floatR'>");
        }
        sb_append_esc(&sb, player_names_account(g_acc.name));
        sb_appendf(&sb, "</a></div><div class='settName' style='width:100%%'>%s<a style='float:right'  onclick='showWindow(13)'>", I18N("generic.clan", "Clan"));
        if (player_names_clan(g_acc.clan)[0]) {
            sb_append(&sb, "[");
            sb_append_esc(&sb, g_acc.clan);
            sb_append(&sb, "]");
        } else {
            sb_append(&sb, "None");
        }
        sb_appendf(&sb, "</a></div><div class='settName'>%s<span class='floatR' style='color:rgba(255,255,255,0.8)'>%d</span></div></div>",
                   I18N("generic.level", "Level"), g_acc.level);
        stats_html(&sb, &g_acc, 0);
    } else if (g_tab_index == PROFILE_TAB_FRIENDS) {
        // v10's friends tab: mutual follows from the friends event bus (v10_social.c)
        sb_append(&sb, "<div class='setBodH' style='padding-top:10px;padding-bottom:10px;margin-bottom:20px;'>Friends can send private messages and see eachothers online status.<div style='height:10px;'></div>You can add new friends by following eachother.</div>");
        char *friends = v10_social_window_friends_html();
        sb_append(&sb, friends ? friends : "<div id='friendList'>Loading Friends...</div>");
        free(friends);
    } else if (g_tab_index == PROFILE_TAB_SETTINGS) { // deob-10.0.3.js:247656-247685
        kid_settings_refresh(rerender_windows); // GET /account/kid once; the answer re-renders this tab
        sb_append(&sb, "<div class='setHedS' style='margin-top:10px;'>Account Settings</div>");
        if (g_acc.premium) {
            sb_append(&sb, "<div class='setBodH'><div class='settName'>Display Name<div class='updDisN' onclick='clearName();' style='background-color: var(--red);'>Clear</div><div class='updDisN' onclick='changeName();'>Update</div><input type='search' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false' id='displayN' class='smlInput' placeholder='");
            sb_append_esc(&sb, g_acc.alias);
            sb_append(&sb, "'><div id='aliasRes'></div></div>");
        }
        const int flg = g_acc.stats && cJSON_IsNumber(cJSON_GetObjectItem(g_acc.stats, "flg")) ? (int) stat_num("flg") : -1;
        sb_appendf(&sb, "<div class='settName'>%s<select onchange='selectFlag(this.value)' class='inputGrey2' style='padding:4px;margin-top:4px'><option value='-1' %s>None</option>",
                   I18N("windows.loadout.flag", "Country Flag"), flg == -1 ? "selected" : "");
        for (int i = 0; i < ACCOUNT_FLAG_COUNT; i++) sb_appendf(&sb, "<option value='%d' %s>%s</option>", i, flg == i ? "selected" : "", ACCOUNT_FLAGS[i].name);
        sb_append(&sb, "</select></div>");
        sb_appendf(&sb, "<div class='settName'>%s<div class='updDisN' onclick='selectBadge(-1);' style='background-color: var(--red);'>Clear</div><div class='updDisN' onclick='selectBadgePop();'>Select</div>",
                   I18N("windows.loadout.badge", "Custom Badge"));
        if (badge_get(stat_bdg())) { // v7 index.js:5651: the selected badge's image
            char url[256];
            badge_url(stat_bdg(), url, sizeof(url));
            sb_appendf(&sb, "<img draggable='false' src='%s' class='customBadge'>", url);
        }
        sb_append(&sb, "</div>");
        sb_appendf(&sb, "<div class='settName'>Hide Detailed Online Status<label class='switch' style='margin-left:10px'><input type='checkbox' onclick='toggleStatus(this.checked)'%s><span class='slider'></span></label></div>", stat_num("hds") ? " checked" : "");
        // Promotional Emails (deob-10.0.3.js:247677-247678): checked from the cached preference (promo_emails_set)
        sb_appendf(&sb, "<div class='settName'>%s<label class='switch' style='margin-left:10px'><input type='checkbox' onclick='togglePromoEmails(this.checked)'%s><span class='slider'></span></label><div style='font-size:16px;line-height:1.8;color:rgba(255,255,255,0.4);margin-top:4px'>%s</div></div></div>",
                   I18N("emailPrefs.settings.label", "Promotional Emails"), g_promo_emails == 1 ? " checked" : "",
                   I18N("emailPrefs.settings.desc", "Receive news about events, game updates and season releases by email"));
        if (!g_acc.hack) {
            sb_append(&sb, "<div class='setHedS'>Account Security</div><div class='setBodH' style='padding-bottom:10px;'><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px'>You can update your password every 30 minutes. <span style='color:#fff'>Never share your password</span> with anyone and keep it in a secure place</div><input type='password' id='oldPass' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='Current Password' /><input type='password' id='newPass' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='New Password' /><input type='password' id='repNPass' class='smlInput' style='width:82%;margin-bottom:8px' placeholder='Repeat Password' /><div class='subPassChg' onclick='changePass()'>");
            sb_append(&sb, I18N("generic.submit", "Submit"));
            sb_append(&sb, "</div><div id='newPRes' style='color:rgba(255,255,255,0.5)'></div>");
            // 2FA: v7 offers it from level 20 (or dev / partner / featured); KrunkNative always does
            if (backend_enabled() || g_scenario) email_html(&sb); // KrunkNative email verification (v7.1.4 has none)
            sb_appendf(&sb, "<div class='settName' style='margin-bottom:20px;'></div><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px;margin-bottom: 10px;'>To further protect your account, you can add enable <span style='color:#fff'>2FA</span>:<div class='subPassChg' onclick='%s' style='float: right;margin-top: -8px;'>%s</div></div>",
                       g_acc.has_2fa ? "dact2FA()" : "act2FA()", g_acc.has_2fa ? "Disable" : "Enable");
            // the usernames row (deob-10.0.3.js:247681): Manage opens the usernames popup (v10_misc_popups.c)
            sb_appendf(&sb, "<div class='settName' style='margin-bottom:20px;'></div><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px;margin-bottom: 10px;'>%s <span style='color:#fff'>",
                       I18N("account.usernames", "You sign in with your active username:"));
            sb_append_esc(&sb, player_names_account(g_acc.name));
            sb_appendf(&sb, "</span><div class='subPassChg' onclick='manageAccNames()' style='float: right;margin-top: -8px;'>%s</div></div></div>",
                       I18N("account.usernames.button", "Manage"));
        }
        // Privacy & Age (deob-10.0.4.js:216008-216011): the kid section mounts between Account Security and Account Privacy
        kid_settings_html(&sb);
        // Account Privacy (deob-10.0.3.js:247684): "Processing Deletion" with Cancel (requestDeletion(0)) while
        // acc.reqDel is set, else Delete Account with Request Deletion. kn_brain's "rqd" (account_life.c).
        sb_appendf(&sb, "<div class='setHedS'>Account Privacy</div><div class='setBodH' style='padding-bottom:15px;'><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px'>%s</div><div style='height:10px;'></div><div style='margin-top:10px'>",
                   I18N("account.delete", "You may request to delete your account, if the request is not cancelled within 14 days the account and all associated data will be permanently removed."));
        if (g_req_del)
            sb_appendf(&sb, "Processing Deletion <a class='menuLink' onclick='requestDeletion(0)' style='float:right;font-size:20px'>%s</a></div></div>",
                       I18N("generic.cancel", "Cancel"));
        else
            sb_append(&sb, "Delete Account <a class='menuLink' onclick='requestDeletion(1)' style='float:right;font-size:20px'>Request Deletion</a></div></div>");
    } else if (g_tab_index == PROFILE_TAB_CONNECTIONS) {
        connections_html(&sb);
    }
    sb_appendf(&sb, "<div style='height:15px;margin-top:30px;border-top:4px solid rgba(255, 255, 255, 0.8);'></div><a class='menuLink' onclick='logoutAcc()'>%s</a>",
               I18N("generic.logout", "Logout"));
    if (!g_logout_sessions)
        sb_appendf(&sb, "<a style='float:right' class='menuLink' onclick='logoutSessions(this)'>%s</a>", I18N("generic.logout.sessions", "Logout of all Sessions"));
    return sb_finish(&sb);
}

// renderRequirement(level, text)
static char *requirement(const int level, const char *text) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='reqW'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LEVEL %d REQUIRED</div><div class='reqWTxt'>%s</div></div>", level, text);
    return sb_finish(&sb);
}

// ------------------------------------------------------------------ WP-E part 2c: badges

// genericPopup(html, cls): #genericPop over #popupBack
static void generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    set_display("popupHolder", "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

static void badge_url(const int index, char *out, const size_t n) {
    char tex[96];
    html_dom_asset_url(badge_texture(index, tex, sizeof(tex)), out, n); // Game.badges[i].url
}

// account.hasSkin(skin): the account's skins (data slot 10, [{ind, cnt}])
static int badge_has_skin(void *ud, const int skin) {
    (void) ud;
    const cJSON *skins = g_acc.data ? cJSON_GetArrayItem(g_acc.data, 10) : NULL, *e;
    cJSON_ArrayForEach(e, skins) {
        const cJSON *ind = cJSON_GetObjectItemCaseSensitive(e, "ind");
        if (cJSON_IsNumber(ind) && ind->valueint == skin) return 1;
    }
    return 0;
}

// claimable && req(account, game) (deob-10.0.3.js:245511-245534): the rules read the account's facts
// the client has, its badges, skins and Battle Pass (v10's KrunkCup and Season 10 pass badges)
static int badge_claimable(const int index) {
    const BadgeDef *b = badge_get(index);
    if (!b || !b->claimable || account_owns_badge(index)) return 0;
    int owned[BADGE_COUNT], count = 0;
    const cJSON *earned = g_logged_in ? g_acc.badges : NULL, *e;
    cJSON_ArrayForEach(e, earned) if (cJSON_IsNumber(e) && count < BADGE_COUNT) owned[count++] = e->valueint;
    const SeasonBadgeAccount facts = {.season = SEASON_CURRENT, .now_s = (int64_t) time(NULL), .bp_tier = battlepass_ui_tier(),
                                      .bpp = battlepass_ui_bpp(), .badges = owned, .badge_count = count, .has_skin = badge_has_skin};
    return badge_rule_met(index, g_acc.stats, &facts);
}

// window.selectBadgePop(): the Badge Collection (claimable first, then owned, then the rest;
// v7 sorts twice with a stable sort, so each group keeps the badges.js order)
static void select_badge_pop(void) {
    if (!g_logged_in) return;
    int claimable = 0, owned = 0;
    for (int i = 0; i < BADGE_COUNT; i++) {
        claimable += badge_claimable(i);
        owned += account_owns_badge(i);
    }
    if (!owned && claimable <= 0) {
        popup_response(0, "You dont own any badges", NULL); // io_showPopup(null, ...)
        return;
    }
    StrBuf sb = {0};
    sb_append(&sb, "<div class='imageListHeader'>Badge Collection</div><div id='skinList' style='margin-top:18px;text-align: center;'>");
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < BADGE_COUNT; i++) {
            const int own = account_owns_badge(i), claim = badge_claimable(i);
            if ((pass == 0 && !claim) || (pass == 1 && (claim || !own)) || (pass == 2 && (claim || own))) continue;
            const BadgeDef *b = badge_get(i);
            char url[256];
            badge_url(i, url, sizeof(url));
            sb_append(&sb, "<div class='badgeCard'");
            if (own) sb_appendf(&sb, " onclick='selectBadge(%d)'", i);
            else if (claim) sb_appendf(&sb, " onclick='claimBadge(%d)' style='outline: 2px inset #FBC02D;'", i);
            sb_appendf(&sb, "><img class='badgeImg' src='%s'><div class='badgeName'>", url);
            sb_append_esc(&sb, b->name && b->name[0] ? b->name : "Unknown");
            sb_append(&sb, "</div><div class='badgeDesc'>");
            sb_append_esc(&sb, b->desc && b->desc[0] ? b->desc : "No Description");
            sb_append(&sb, "</div>");
            if (claim) sb_append(&sb, "<div class='badgeBound'><span class='material-icons' style='font-size:30px;color:#FBC02D;'>lock_open</span></div>");
            else if (!own) sb_append(&sb, "<div class='badgeBound'><span class='material-icons' style='font-size:30px;color:#fff;'>lock</span></div>");
            sb_append(&sb, "</div>");
        }
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "badgePop");
    free(html);
}

// ------------------------------------------------------------------ WP-E part 2c: challenges

static const char *weapon_name_at(const int index) {
    return index >= 0 && index < WEAPON_COUNT && rules_weapon(index) ? rules_weapon(index)->name : NULL;
}

// challenges.updateNames(): N or i18n chall.name.<ky>
static void chal_name(const cJSON *c, char *out, const size_t n) {
    challenge_name(c, weapon_name_at, out, n);
    if (!out[0]) { // i18n.t of a missing key prints the key
        const cJSON *dat = cJSON_GetObjectItem(c, "DAT");
        const char *ky = cJSON_GetStringValue(cJSON_GetObjectItem(dat, "ky"));
        snprintf(out, n, "chall.name.%s", ky ? ky : "");
    }
}

static double cnum(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : 0.0;
}

static int chas(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsNumber(v) || cJSON_IsBool(v);
}

static const char *ctype(const cJSON *c) {
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(c, "T"));
    return t ? t : "";
}

static void cap_first(StrBuf *sb, const char *s) { // UTILS.capFirst
    if (!s || !s[0]) return;
    char first[2] = {(char) toupper((unsigned char) s[0]), 0};
    sb_append_esc(sb, first);
    sb_append_esc(sb, s + 1);
}

// windows[32].genChallengeCard(challenge, marginTop)
static void chal_card(StrBuf *sb, const cJSON *c, const int margin) {
    const cJSON *dat = cJSON_GetObjectItem(c, "DAT"), *rew = cJSON_GetObjectItem(c, "REW");
    const int boss = cnum(dat, "boss") != 0.0;
    const int done = cnum(c, "PT") != 0.0;
    const char *ky = cJSON_GetStringValue(cJSON_GetObjectItem(dat, "ky"));
    const int time_key = ky && !strcmp(ky, "time");
    const double val = cnum(dat, "val"), pv = cnum(c, "PV");
    // REW.sk: up to 3 reward skins, shown last to first
    int skins[3] = {-1, -1, -1}, skin_count = 0;
    const cJSON *sk = cJSON_GetObjectItem(rew, "sk");
    if (cJSON_IsNumber(sk)) skins[skin_count++] = sk->valueint;
    const cJSON *it;
    cJSON_ArrayForEach(it, (cJSON_IsArray(sk) ? sk : NULL)) if (skin_count < 3 && cJSON_IsNumber(it)) skins[skin_count++] = it->valueint;
    char icon[256], path[64], name[160], text[64];
    snprintf(path, sizeof(path), "img/chals/icon_%d.png", (int) cnum(c, "IC"));
    ui_img_url(path, icon, sizeof(icon)); // UTILS.assetsUrl: KrunkNative's ui/ copy (fetch_season_images.sh)
    chal_name(c, name, sizeof(name));
    sb_appendf(sb, "<div class='chalCard' style='margin-top:%dpx;%s'><img class='chalImgC' src='%s'  style='%s%s' />", margin,
               boss ? "box-shadow: 0 0 14px #ffa000;background:rgb(79 50 0 / 40%);border:4px solid #ff9c00;" : "", icon,
               boss ? "border: 4px solid #ff9c00;" : "", done ? "filter:grayscale(1);brightness(0.8);" : "");
    if (done) sb_append(sb, "<div class='challComplB'><span class='material-icons' style='font-size:110px;color:#38cd3a;'>done</span></div>");
    sb_append(sb, "<div class='chalRight'>");
    sb_append_esc(sb, name);
    if (cnum(c, "TR") > 0) {
        readable_time(text, sizeof(text), cnum(c, "TR"));
        sb_appendf(sb, "<div class='chalExpr'>%s left</div>", text);
    }
    if (done) snprintf(text, sizeof(text), "Completed");
    else if (time_key) readable_time(text, sizeof(text), val);
    else snprintf(text, sizeof(text), "%.15g/%.15g", pv, val);
    sb_appendf(sb, "<div class='chalXPBar'><div class='chalXPVal'>%s</div><div class='chalXPBarC' style='width:%.15g%%;'></div></div></div>", text,
               done ? 100.0 : time_key || val == 0.0 ? 0.0 : pv / val * 100.0);
    const double kr = cnum(rew, "kr"), jnk = cnum(rew, "jnk");
    sb_appendf(sb, "<div class='chalRews' style='%s'><div class='chalRewV' style='%s'><span style='color:white'>+</span>%.15g <span style='color:#fbc02d'>KR</span></div>",
               boss ? "border-left: 3px solid #ff9c00;" : "", kr ? "" : "filter:grayscale(1);", kr);
    sb_appendf(sb, "<div class='chalRewV' style='%s'><span style='color:white'>+</span>%.15g <span class='material-icons' style='color:#cc51f8;font-size:29px;vertical-align:top;'>plumbing</span></div><div>",
               jnk ? "" : "filter:grayscale(1);", jnk);
    for (int i = 0; i < 3; i++) {
        const int index = skins[2 - i];
        const ViewerSkin *skin = index >= 0 ? viewer_skin(index) : NULL;
        sb_append(sb, "<div class='chalRewSkn' style='");
        if (skin) {
            char url[256];
            store_skin_preview(index, url, sizeof(url));
            const int r = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
            sb_appendf(sb, "background-image:url(%s);border:2px solid %s;' onclick='viewItem(%d)'>", url, g_viewer_rarities[r].color, index);
        } else {
            sb_append(sb, "' >");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

// (boss ? 2 : PT) descending, stable (v7's Array.sort)
static int chal_sorted(const cJSON **out, const int max, const char *type, const char *field, const int value) {
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, g_chal) {
        if (strcmp(ctype(c), type) != 0 || n >= max) continue;
        if (field && (!chas(cJSON_GetObjectItem(c, "DAT"), field) || (int) cnum(cJSON_GetObjectItem(c, "DAT"), field) != value)) continue;
        out[n++] = c;
    }
    for (int i = 1; i < n; i++) {
        const cJSON *x = out[i];
        const double kx = cnum(cJSON_GetObjectItem(x, "DAT"), "boss") ? 2.0 : cnum(x, "PT");
        int j = i - 1;
        while (j >= 0 && (cnum(cJSON_GetObjectItem(out[j], "DAT"), "boss") ? 2.0 : cnum(out[j], "PT")) < kx) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = x;
    }
    return n;
}

static void chal_header(StrBuf *sb, const char *image, const int margin, const char *title, const int done, const int total, const int back,
                        const char *extra_style) {
    char url[256], path[96];
    snprintf(path, sizeof(path), "img/chals/%s.png", image);
    ui_img_url(path, url, sizeof(url));
    sb_appendf(sb, "<div class='challHeadrI' style='background-image:url(%s);margin-top:%dpx;%s'>", url, margin, extra_style ? extra_style : "");
    if (back) sb_append(sb, "<div class='challHeadrBck' onclick='windows[32].closeChallengeCat();'><span class='material-icons challBackArr'>arrow_back</span></div>");
    sb_append(sb, "<div class='challHeadrT'>");
    sb_append_esc(sb, title);
    sb_appendf(sb, "</div><div class='challHeadrS'>%d<span style='color:#fff;'>/%d</span></div></div>", done, total);
}

#define CHAL_MSG "<div class='challCrdMsg' style='margin-bottom:25px;font-size:27px;color:#9f9f9f;'>Abandoning a match will result in the loss of challenge progress.</div>"
#define CHAL_LIST_MAX 256

// The cards of one list (the v7 category views and KrunkNative's daily / weekly sections).
static void chal_cards(StrBuf *sb, const char *type, const char *field, const int value, const char *empty) {
    const cJSON *list[CHAL_LIST_MAX];
    const int n = chal_sorted(list, CHAL_LIST_MAX, type, field, value);
    sb_append(sb, "<div style='margin-top:10px;'>");
    for (int i = 0; i < n; i++) chal_card(sb, list[i], i ? 20 : 0);
    if (!n && empty) sb_appendf(sb, "<div class='challCrdMsg'>%s</div>", empty);
    sb_append(sb, "</div>");
}

static void chal_count(const char *type, const char *field, const int value, int *done, int *total) {
    *done = *total = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, g_chal) {
        if (strcmp(ctype(c), type) != 0) continue;
        const cJSON *dat = cJSON_GetObjectItem(c, "DAT");
        if (field && (!chas(dat, field) || (int) cnum(dat, field) != value)) continue;
        (*total)++;
        *done += cnum(c, "PT") != 0.0;
    }
}

// windows[32].gen() (v7.1.4 index.js:8742) for an account; guests keep the static LOGIN REQUIRED
// page. config.minChalLevel is 1 in v7 (30 in v3.1.1), so every account gets the list. The
// daily challenges (and KrunkNative's weekly ones) are in the Battle Pass window's Challenges
// tab, as in v7 (battlepass_ui.c). v7's Event Challenges section needs an active
// config.events entry (UTILS.hasActiveEvents); v7.1.4's config.events is empty, so it never
// shows and is not ported.
static char *gen_challenges(void) {
    if (g_acc.level < 1) return requirement(1, "for Challenges");
    if (!g_chal) {
        if (!g_chal_requested) {
            g_chal_requested = 1;
            backend_social_send_text("[\"chg\"]");
        }
        return strdup("Loading");
    }
    if (cJSON_GetArraySize(g_chal) == 0) return strdup("<span style='color: #fff;'>No active challenges.</span>");
    StrBuf sb = {0};
    int done, total;
    if (!g_chal_typ[0]) {
        const int margin = 0;
        // Map Challenges: one card per map that has some
        chal_count("map", NULL, 0, &done, &total);
        chal_header(&sb, "hed_1", margin, "Map Challenges", done, total, 0, NULL);
        sb_append(&sb, "<div style='margin-top:10px;text-align:center;'>");
        for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) { // i: Krunker's mapIndex (the challenge's id)
            const int catalog = map_catalog_official_index(i);
            const MapCatalogEntry *m = catalog >= 0 ? map_catalog_at((size_t) catalog) : NULL;
            int d, t;
            chal_count("map", "mp", i, &d, &t);
            if (!m || !t) continue;
            char path[64], url[256];
            ui_img_url(map_catalog_thumb(m, path, sizeof(path)), url, sizeof(url));
            sb_appendf(&sb, "<div class='challMapCrd' style='background-image:url(%s)' onmouseenter='playTick()' onclick='windows[32].openChallengeCat(&quot;map&quot;,%d);'><div class='challMapCvr'></div><div class='challMapName blackShad'>", url, i);
            cap_first(&sb, m->name);
            sb_appendf(&sb, "</div><div class='challMapPrg'>%d<span style='color:#fff;'>/%d</span></div></div>", d, t);
        }
        sb_append(&sb, "</div>");
        chal_count("weapon", NULL, 0, &done, &total);
        chal_header(&sb, "hed_2", 30, "Weapon Challenges", done, total, 0, NULL);
        sb_append(&sb, "<div style='margin-top:10px;text-align:center;'>");
        for (int i = 0; i < WEAPON_COUNT; i++) {
            const Weapon *w = rules_weapon(i);
            int d, t;
            chal_count("weapon", "wep", i, &d, &t);
            if (!w || w->type == 3 || w->custom_only || !t) continue; // weaponTypes.EQUIPMENT, customOnly
            char path[96], url[256];
            snprintf(path, sizeof(path), "img/chals/%s.png", w->src ? w->src : "noimg");
            ui_img_url(path, url, sizeof(url));
            sb_appendf(&sb, "<div class='challMapCrd' onmouseenter='playTick()' onclick='windows[32].openChallengeCat(&quot;weapon&quot;,%d);'><img class='challRotImg' style='background-image:url(%s);background-size:contain;background-repeat:no-repeat;' /><div class='challMapCvr'></div><div class='challMapName blackShad'>", i, url);
            cap_first(&sb, w->name);
            sb_appendf(&sb, "</div><div class='challMapPrg'>%d<span style='color:#fff;'>/%d</span></div></div>", d, t);
        }
        sb_append(&sb, "</div>");
        chal_count("nightmare", NULL, 0, &done, &total);
        chal_header(&sb, "hed_4", 30, "Nightmare Challenges", done, total, 0, NULL);
        chal_cards(&sb, "nightmare", NULL, 0, "Check back later for new nightmare challanges!"); // sic (v7)
    } else if (!strcmp(g_chal_typ, "map")) {
        const int catalog = g_chal_ind >= 0 && g_chal_ind < MAP_CATALOG_OFFICIAL_COUNT ? map_catalog_official_index(g_chal_ind) : -1;
        const MapCatalogEntry *m = catalog >= 0 ? map_catalog_at((size_t) catalog) : NULL;
        char title[96];
        snprintf(title, sizeof(title), "%s Challenges", m ? m->name : "");
        chal_count("map", "mp", g_chal_ind, &done, &total);
        chal_header(&sb, "hed_1", 0, title, done, total, 1, NULL);
        sb_append(&sb, CHAL_MSG);
        chal_cards(&sb, "map", "mp", g_chal_ind, "Check back later for new challenges!");
    } else if (!strcmp(g_chal_typ, "weapon")) {
        const char *wname = weapon_name_at(g_chal_ind);
        const Weapon *w = g_chal_ind >= 0 && g_chal_ind < WEAPON_COUNT ? rules_weapon(g_chal_ind) : NULL;
        char title[96];
        snprintf(title, sizeof(title), "%s Challenges", wname ? wname : "");
        chal_count("weapon", "wep", g_chal_ind, &done, &total);
        chal_header(&sb, w && w->src ? w->src : "noimg", 0, title, done, total, 1, "filter: saturate(0.8);");
        sb_append(&sb, CHAL_MSG);
        chal_cards(&sb, "weapon", "wep", g_chal_ind, "Check back later for new challenges!");
    }
    return sb_finish(&sb);
}

const cJSON *account_challenges(void) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) { // account.activeChallenges of the krunker.io account ("ua" slot 38)
        const cJSON *c = active_account() ? cJSON_GetObjectItemCaseSensitive(g_kio.obj, "activeChallenges") : NULL;
        return cJSON_IsArray(c) ? c : NULL;
    }
    return g_logged_in ? g_chal : NULL;
}

void account_challenges_request(void) {
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) return; // (the krunker.io account's come with its "a" / "ua")
    if (!g_logged_in || g_chal || g_chal_requested) return;
    g_chal_requested = 1;
    backend_social_send_text("[\"chg\"]");
}

void account_challenge_name(const cJSON *c, char *out, const size_t n) { chal_name(c, out, n); }

static cJSON *chal_by_ci(const double ci) {
    cJSON *c;
    cJSON_ArrayForEach(c, g_chal) if (cnum(c, "CI") == ci) return c;
    return NULL;
}

static void chal_set(cJSON *c, const char *key, const double v) {
    if (cJSON_GetObjectItem(c, key)) cJSON_ReplaceItemInObject(c, key, cJSON_CreateNumber(v));
    else cJSON_AddNumberToObject(c, key, v);
}

// ------------------------------------------------------------------ WP-E part 2c: medal counts

// KrunkNative: the account's medal counts (kn_brain "mdl") as the profile's last stat tab (STAT_TAB_MEDALS)
static void medal_tab_html(StrBuf *sb) {
    if (!g_medal_counts) {
        sb_append(sb, "<div class='pSt'>Medals<strong>...</strong></div>");
        return;
    }
    int any = 0;
    for (int i = 0; i < PROGRESSION_MEDAL_COUNT; i++) {
        const cJSON *v = cJSON_GetObjectItem(g_medal_counts, g_progression_medals[i]);
        if (!cJSON_IsNumber(v) || v->valuedouble <= 0) continue;
        pst_num(sb, g_progression_medals[i], v->valuedouble, 0);
        any = 1;
    }
    if (!any) sb_append(sb, "<div style='color:rgba(255,255,255,0.5);font-size:20px;padding:10px;'>No medals yet</div>");
}

static void medal_tab_update(void) {
    HtmlElem tab = html_dom_query(0, ".statTabB5");
    if (!tab) return;
    StrBuf sb = {0};
    medal_tab_html(&sb);
    char *html = sb_finish(&sb);
    html_dom_set_html(tab, html);
    free(html);
}

// ------------------------------------------------------------------ WP-E part 2c: frames

// kn_brain's badge / challenge / medal frames (doc/backend.md 7.4); 1 = handled.
static int progress_frame(const char *label, const cJSON *frame) {
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1);
    if (!strcmp(label, "bdgr")) { // io_badgeUpdate(bdg)
        if (g_logged_in && cJSON_IsNumber(a1)) set_stat("bdg", a1->valueint);
        ui_loadout_clear_pops(); // clearPops()
        menu_rerender_if_open(5);
        menu_update_class_box();
    } else if (!strcmp(label, "cbdgr")) { // io_badgeClaimResponse(badge)
        if (g_logged_in && cJSON_IsNumber(a1)) add_badge(a1->valueint);
        popup_response(1, NULL, "Badge Claimed!");
    } else if (!strcmp(label, "nbdgr")) { // io_badgeClaimMultiRes(badges): checkBadges at the match end
        const cJSON *b;
        cJSON_ArrayForEach(b, (g_logged_in ? a1 : NULL)) if (cJSON_IsNumber(b)) add_badge(b->valueint);
        popup_response(1, NULL, "You unlocked new badges");
    } else if (!strcmp(label, "chgL")) { // the "chg" list: account.activeChallenges
        g_chal_requested = 0;
        if (!g_logged_in || !cJSON_IsArray(a1)) return 1;
        cJSON_Delete(g_chal);
        g_chal = cJSON_Duplicate(a1, 1);
        menu_rerender_if_open(33);
        battlepass_ui_challenges_changed(); // windows[5] tab 1: the daily (and weekly) cards
    } else if (!strcmp(label, "chg")) { // io_updateChallenges([done, CI, PV, ...])
        for (int i = 0; g_chal && i + 2 < cJSON_GetArraySize(a1); i += 3) {
            cJSON *c = chal_by_ci(cJSON_GetNumberValue(cJSON_GetArrayItem(a1, i + 1)));
            if (!c) continue;
            chal_set(c, "PV", cJSON_GetNumberValue(cJSON_GetArrayItem(a1, i + 2)));
            chal_set(c, "PT", cJSON_GetNumberValue(cJSON_GetArrayItem(a1, i)));
        }
        menu_rerender_if_open(33); // if (!inGame) updateWindow(33)
        battlepass_ui_challenges_changed();
    } else if (!strcmp(label, "chgC")) { // io_challengeCompleted: the end screen's "Completed!" cards
        const cJSON *c;
        cJSON_ArrayForEach(c, a1) {
            char name[160];
            chal_name(c, name, sizeof(name));
            hud_ui_end_challenge_completed((int) cnum(c, "IC"), name);
            const cJSON *sk = cJSON_GetObjectItem(cJSON_GetObjectItem(c, "REW"), "sk"), *it; // matchRewardSummary.skins
            if (cJSON_IsNumber(sk)) hud_ui_add_reward_skin(sk->valueint);
            cJSON_ArrayForEach(it, (cJSON_IsArray(sk) ? sk : NULL)) if (cJSON_IsNumber(it)) hud_ui_add_reward_skin(it->valueint);
            cJSON *mine = chal_by_ci(cnum(c, "CI"));
            if (mine) chal_set(mine, "PT", 1);
        }
    } else if (!strcmp(label, "chlR")) { // io_updateChallengeRewards(skins, funds, junk, jnk, kr)
        if (!g_logged_in) return 1;
        const cJSON *funds = cJSON_GetArrayItem(frame, 2), *it;
        if (cJSON_IsNumber(funds)) account_set_funds(funds->valuedouble);
        cJSON_ArrayForEach(it, (cJSON_IsArray(a1) ? a1 : NULL)) if (cJSON_IsNumber(it)) account_add_skin(it->valueint);
    } else if (!strcmp(label, "mdl")) {
        if (!g_logged_in || !cJSON_IsObject(a1)) return 1;
        cJSON_Delete(g_medal_counts);
        g_medal_counts = cJSON_Duplicate(a1, 1);
        medal_tab_update();
    } else {
        return 0;
    }
    return 1;
}

// windows[32] and the badge handlers; -1 = not ours
static int progress_handle(const char *name, const HtmlArg *args, const int argc) {
    const int a0 = argc > 0 && (args[0].kind == HTML_ARG_NUM || args[0].kind == HTML_ARG_BOOL) ? (int) args[0].num
                 : argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? atoi(args[0].str) : -1;
    char frame[48];
    if (!strcmp(name, "selectBadge")) { // if (account) io.send("bdg", parseInt(i))
        if (g_logged_in) {
            snprintf(frame, sizeof(frame), "[\"bdg\",%d]", a0);
            backend_social_send_text(frame);
        }
    } else if (!strcmp(name, "claimBadge")) { // io.send("cbdg", parseInt(i))
        if (g_logged_in) {
            snprintf(frame, sizeof(frame), "[\"cbdg\",%d]", a0);
            backend_social_send_text(frame);
        }
    } else if (!strcmp(name, "selectBadgePop")) {
        select_badge_pop();
    } else if (!strcmp(name, "windows.32.openChallengeCat")) { // (typ, ind)
        const char *typ = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? args[0].str : "";
        snprintf(g_chal_typ, sizeof(g_chal_typ), "%s", typ);
        g_chal_ind = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : 0;
        menu_show_window(33, 1); // menuWindow.scrollTop = 0; updateWindow(33)
    } else if (!strcmp(name, "windows.32.closeChallengeCat")) {
        g_chal_typ[0] = 0;
        menu_show_window(33, 1);
    } else if (!strcmp(name, "windows.32.openHome")) {
        g_chal_typ[0] = 0;
        menu_show_window(33, 0); // showWindow(33)
    } else {
        return -1;
    }
    return 1;
}

// client_bin --render-scenario (ui_scenario.c): state.account = {name, score, funds, clan, premium,
// spendingTier, badges [], stats {}, challenges [], medals {}, raidStats []} stands in for a logged-in account, so the
// account windows (Challenges, Account) can be rendered without kn_brain (WP-E part 2c).
void account_ui_scenario(const cJSON *account) {
    if (!cJSON_IsObject(account)) return;
    clear_account();
    g_logged_in = 1;
    g_scenario = 1;
    g_acc.id = 1;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(account, "name"));
    copy_str(g_acc.name, sizeof(g_acc.name), name ? name : "Player");
    copy_str(g_acc.alias, sizeof(g_acc.alias), g_acc.name);
    const char *clan = cJSON_GetStringValue(cJSON_GetObjectItem(account, "clan"));
    copy_str(g_acc.clan, sizeof(g_acc.clan), clan);
    g_acc.score = cnum(account, "score");
    g_acc.funds = cnum(account, "funds");
    g_acc.junk = cnum(account, "junk");                // the Wallet's Junk card (player_jnk: the krunker.io account's)
    g_acc.ranked_points = cnum(account, "rankedPoints"); // the Wallet's Ranked Points card
    g_acc.spending_tier = (int) cnum(account, "spendingTier"); // the Purchase KR tab's spender-only cards (store_ui.c)
    g_acc.games = cnum(account, "games"); // the per-match Double XP offer (games % 3, double_xp_offer.c)
    g_acc.premium = cnum(account, "premium") != 0.0;
    g_acc.is_dev = cnum(account, "dev") != 0.0; // the player list's Kick / Ban (punishPopup) need DEV rights
    update_level();
    const cJSON *stats = cJSON_GetObjectItem(account, "stats");
    g_acc.stats = cJSON_IsObject(stats) ? cJSON_Duplicate(stats, 1) : cJSON_CreateObject();
    set_badges(account);
    set_dxp(account); // state.account.dxp = {offer: {claimable}}: the end screen's 2x XP button
    const cJSON *chal = cJSON_GetObjectItem(account, "challenges"), *medals = cJSON_GetObjectItem(account, "medals");
    g_chal = cJSON_IsArray(chal) ? cJSON_Duplicate(chal, 1) : NULL;
    g_medal_counts = cJSON_IsObject(medals) ? cJSON_Duplicate(medals, 1) : NULL;
    const cJSON *raids = cJSON_GetObjectItem(account, "raidStats"); // acc.raidStats [{map_id, completions, ...}]
    g_raid_stats = cJSON_IsArray(raids) ? cJSON_Duplicate(raids, 1) : NULL;
    battlepass_ui_scenario(cJSON_GetObjectItem(account, "battlepass")); // {data, progress, tab, end, now}
    const cJSON *skins = cJSON_GetObjectItem(account, "skins");
    if (cJSON_IsArray(skins)) { // the inventory [{ind, cnt}] (data slot 10; funds 5, lastReward 11)
        g_acc.data = cJSON_CreateArray();
        for (int i = 0; i < 49; i++) cJSON_AddItemToArray(g_acc.data, cJSON_CreateNull());
        cJSON_ReplaceItemInArray(g_acc.data, 5, cJSON_CreateNumber(g_acc.funds));
        cJSON_ReplaceItemInArray(g_acc.data, 10, cJSON_Duplicate(skins, 1));
        cJSON_ReplaceItemInArray(g_acc.data, 11, cJSON_CreateNumber(0));
    }
    email_state(cJSON_GetObjectItem(account, "email")); // {email, verified, pending, mail}
    g_acc.has_2fa = cnum(account, "twofa") != 0.0;
    g_tab_index = (int) cnum(account, "tab"); // windows[4].tabIndex
    const cJSON *c2fa = cJSON_GetObjectItem(account, "c2fa"); // ["c2fa", 0, "a2FAc", "otpauth://..."]
    cJSON_Delete(g_scenario_c2fa);
    g_scenario_c2fa = cJSON_IsArray(c2fa) ? cJSON_Duplicate(c2fa, 1) : NULL;
    mark_header();
    v10_menu_account_changed(); // the header and nav now: a scenario renders before account_ui_tick
    menu_update_class_box();
}

void account_ui_scenario_post(void) {
    if (!g_scenario_c2fa) return;
    twofa_res(g_scenario_c2fa);
    cJSON_Delete(g_scenario_c2fa);
    g_scenario_c2fa = NULL;
}

// windows[4].gen() for the krunker.io account (the active one in an official game): v10's Account
// window with what krunker.io tells this client about it (name, level, score, KR, raid stats); its friends,
// settings and connections tabs are krunker.io's own services, which this client does not
// reach: the window has the home tab only, and Logout signs out of krunker.io.
static char *gen_kio_account(const AccountInfo *acc) {
    StrBuf sb = {0};
    sb_append(&sb, "<div style='height:15px;'></div><div class='bigMenTabs'><div class='bigMenTab bigMenTabA' style='font-size:16px' onmouseenter='playTick(0.1);'>"
                   "<span class='material-icons' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span></div></div>");
    sb_append(&sb, "<div class='setHed' style='margin-top:10px;margin-bottom:20px;'>Your Account</div>");
    // v10's #profilePicM gets .blurred (image-rendering pixelated, greyed) unless a premium account shows its own
    // upload. This page shows only the stock icon_0, so it is blurred, as the native account's picture is (above)
    sb_append(&sb, "<div id='profilePicM' class='blurred'><img style='width: inherit;height: inherit;border-radius: inherit;' loading='lazy' src='textures/classes/icon_0.png'></div>");
    sb_append(&sb, "<div class='setBodH' id='knKioAccName' style='display:inline-block;padding-top:10px;vertical-align:top;width:1004px;margin-left:15px;margin-top:0px;'><div class='settName' style='width:100%;margin-top:0px'>");
    sb_append(&sb, I18N("generic.name", "Name"));
    sb_append(&sb, "<span class='floatR' style='color:rgba(255,255,255,0.8)'>");
    sb_append_esc(&sb, player_names_account(acc->name)); // Hide player names: "Player"
    sb_appendf(&sb, "</span></div><div class='settName' style='width:100%%'>%s<span class='floatR' style='color:rgba(255,255,255,0.8)'>",
               I18N("generic.clan", "Clan"));
    if (player_names_clan(acc->clan)[0]) {
        sb_append(&sb, "[");
        sb_append_esc(&sb, acc->clan);
        sb_append(&sb, "]");
    } else {
        sb_append(&sb, "None");
    }
    sb_appendf(&sb, "</span></div><div class='settName'>%s<span class='floatR' style='color:rgba(255,255,255,0.8)'>%d</span></div></div>",
               I18N("generic.level", "Level"), acc->level);
    stats_html(&sb, acc, 1); // its real stats: the official game server's "a" / "ua" data
    sb_appendf(&sb, "<div style='height:15px;margin-top:30px;border-top:4px solid rgba(255, 255, 255, 0.8);'></div><a class='menuLink' id='knKioLogout' onclick='logoutAcc()'>%s</a>",
               I18N("generic.logout", "Logout"));
    return sb_finish(&sb);
}

char *account_window_gen(const int id, const char **header) {
    (void) header;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) { // the krunker.io account: window 5 only (the others need kn_brain)
        const AccountInfo *kio = active_account();
        return kio && id == 5 ? gen_kio_account(kio) : NULL;
    }
    if ((backend_available() != 1 && !g_scenario) || !g_logged_in) return NULL; // the guest captures (LOGIN REQUIRED cards, login form)
    if (id == 5) return gen_account();
    if (id == 33) return gen_challenges();
    if (id == 30) return gen_mail();
    return NULL;
}

void account_window_post(const int id) {
    if (id != 5) return;
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER ? active_account() != NULL : g_logged_in && (backend_available() == 1 || g_scenario)) {
        // windows[4].gen(): this.width = account ? 1280 : 955. The captured style is the guest's
        // 955 px, too narrow for the signed-in profile (its Name / Clan / Level box is 1004 px
        // beside the 181 px picture: it wrapped under it and its right column was clipped).
        const HtmlElem mw = html_dom_by_id("menuWindow");
        if (mw) html_dom_set_style(mw, "width", "1280px");
        return;
    }
    // A guest never gets this window on the v10 page: v10_login_guest_window opens the login popup
    // (v10 windows[4].gen, autoWindowClose) and the template is v10's own, empty one. It is only the
    // frame of the password-reset steps, and a re-render (account update) keeps the step.
    if (!g_logged_in && g_reset.step > 0) reset_render();
}
