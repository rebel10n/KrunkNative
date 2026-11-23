// Krunker v10.0.7's getSkins(account, true) (deob-10.0.7.js:217752-217795) on the game server: what accounts_owns_skin lets
// an account wear on ENT. The free items are everyone's; a developer has every store skin and the game's customSkins;
// everyone else has acc.skins plus the store items whose creators hold the account's lower-cased name, whose streamers
// hold its lower-cased Twitch (tw_twitchname), the K.P.D's (kpd) for a moderator, and a skin maker's (player_skinmaker)
// season 6+ items no creator made and not blocked. The shared rule (store_item_owner_match, shared/src/social_v7.c) is
// the client loadout's too. Run from the repository root (assets/skins.json).
#include <server.h>
#include <server_accounts.h>
#include <server_config.h>
#include <social_v7.h>
#include <shared.h>
#include <cJSON.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// assets/skins.json (v10.0.7)
#define FREE_SPRAY 2482   // "Krunker": free
#define ATARST 73         // creators ["atarst"] (creator "AtarSt"), season 1
#define DUO 1084          // creators ["nxbulah", "kilfy"]
#define EMOJI_BEANIE 2904 // streamers ["kaarson"], season 4
#define CITIZENS 2998     // "Citizen's Arrest": kpd, season 5, no creator
#define PUMPIE 2965       // season 7, no creator, not blocked: a skin maker's
#define DIORAMA 5236      // season 6, creator "Multihawk": no skin maker's
#define BRAINIE 5235      // season 6, no creator, blocked: no skin maker's
#define SANDY 3235        // season 5, no creator: no skin maker's
#define CUSTOM 10000005   // a custom game's studio skin (customSkins, deob-10.0.7.js:17253)

static ClientConnection g_client;

static void account(const char *json) {
    memset(&g_client, 0, sizeof(g_client));
    g_client.fd = -1;
    cJSON *a = cJSON_Parse(json);
    const cJSON *id = cJSON_GetObjectItem(a, "id");
    g_client.acct.account_id = cJSON_IsNumber(id) ? id->valueint : 0;
    snprintf(g_client.acct.username, sizeof(g_client.acct.username), "%s", cJSON_GetStringValue(cJSON_GetObjectItem(a, "username")) ? cJSON_GetStringValue(cJSON_GetObjectItem(a, "username")) : "");
    if (cJSON_IsTrue(cJSON_GetObjectItem(a, "dev"))) g_client.acct.flags |= NET_PLAYER_INFO_DEV;
    if (cJSON_IsTrue(cJSON_GetObjectItem(a, "mod"))) g_client.acct.flags |= NET_PLAYER_INFO_MOD;
    accounts_load_skins(&g_client, a);
    accounts_load_owner(&g_client, a);
    cJSON_Delete(a);
}

static int owns(const int skin) { return accounts_owns_skin(&g_client, skin); }

int main(void) {
    static Server server;
    g_server = &server;
    g_gs_config.enforce_skins = 1;

    printf("Running the store table...\n");
    CHECK(store_item_free(FREE_SPRAY) && !store_item_free(ATARST), "free: the Krunker spray, not AtarSt's item");
    CHECK(store_item_exists(0) && store_item_exists(8000) && !store_item_exists(-1) && !store_item_exists(60000), "store_item_exists");
    const StoreOwner atarst = {"AtarSt", NULL, 0, 0};
    CHECK(store_item_owner_match(ATARST, &atarst), "item.creators has the lower-cased name (AtarSt -> atarst)");
    const StoreOwner kilfy = {"Kilfy", NULL, 0, 0};
    CHECK(store_item_owner_match(DUO, &kilfy) && !store_item_owner_match(ATARST, &kilfy), "a second creator's name");
    const StoreOwner zed = {"someone", "KaarSon", 0, 0};
    CHECK(store_item_owner_match(EMOJI_BEANIE, &zed), "item.streamers has the lower-cased Twitch");
    const StoreOwner no_tw = {"kaarson", NULL, 0, 0};
    CHECK(!store_item_owner_match(EMOJI_BEANIE, &no_tw), "a streamer's item is not the account named like the streamer");
    const StoreOwner tw_named = {"x", "atarst", 0, 0};
    CHECK(!store_item_owner_match(ATARST, &tw_named), "a creator's item is not the Twitch named like the creator");
    const StoreOwner officer = {"x", NULL, 1, 0}, maker = {"x", NULL, 0, 1};
    CHECK(store_item_owner_match(CITIZENS, &officer) && !store_item_owner_match(CITIZENS, &maker), "item.kpd: an officer's");
    CHECK(store_item_owner_match(PUMPIE, &maker) && !store_item_owner_match(PUMPIE, &officer), "season 7, no creator: a skin maker's");
    CHECK(!store_item_owner_match(DIORAMA, &maker), "season 6 with a creator: not a skin maker's");
    CHECK(!store_item_owner_match(BRAINIE, &maker), "season 6, blocked: not a skin maker's");
    CHECK(!store_item_owner_match(SANDY, &maker), "season 5: not a skin maker's");
    const StoreOwner empty = {"", "", 0, 0};
    CHECK(!store_item_owner_match(ATARST, &empty) && !store_item_owner_match(EMOJI_BEANIE, &empty), "no name, no Twitch: nothing");

    printf("Running a guest...\n");
    account("{\"id\":0}");
    CHECK(owns(FREE_SPRAY) && !owns(ATARST) && !owns(PUMPIE) && !owns(CUSTOM), "a guest wears the free items only");

    printf("Running a plain account...\n");
    account("{\"id\":5,\"username\":\"Plain\",\"data\":[0,0,0,0,0,0,0,\"\",0,0,[{\"ind\":111,\"cnt\":1}]]}");
    CHECK(owns(FREE_SPRAY) && owns(111), "the free items and acc.skins");
    CHECK(!owns(ATARST) && !owns(EMOJI_BEANIE) && !owns(CITIZENS) && !owns(PUMPIE) && !owns(CUSTOM) && !owns(112),
          "nothing else");

    printf("Running a creator...\n");
    account("{\"id\":6,\"username\":\"AtarSt\",\"skins\":[]}");
    CHECK(owns(ATARST) && !owns(DUO) && !owns(EMOJI_BEANIE), "item.creators by the account name");

    printf("Running a streamer...\n");
    account("{\"id\":7,\"username\":\"zz\",\"twitch\":\"Kaarson\",\"skins\":[]}");
    CHECK(owns(EMOJI_BEANIE) && !owns(ATARST), "item.streamers by the account info's twitch");
    CHECK(!strcmp(g_client.acct.twitch, "Kaarson"), "acct.twitch (%s)", g_client.acct.twitch);
    account("{\"id\":7,\"username\":\"zz\",\"twitch\":null,\"data\":[0,0,0,0,0,0,0,\"\",0,0,[],0,0,0,0,0,{},0,null,0,0,0,0,0,0,0,0,0,0,\"zz\","
            "\"2020-01-01\",\"kaarson\"]}");
    CHECK(owns(EMOJI_BEANIE), "no twitch key: data[31] (tw_twitchname)");
    account("{\"id\":7,\"username\":\"zz\",\"skins\":[]}");
    CHECK(!owns(EMOJI_BEANIE) && !g_client.acct.twitch[0], "no Twitch: no streamer items");

    printf("Running a K.P.D officer...\n");
    account("{\"id\":8,\"username\":\"cop\",\"mod\":true,\"skins\":[]}");
    CHECK(owns(CITIZENS) && !owns(PUMPIE) && !owns(ATARST), "item.kpd for a moderator");

    printf("Running a skin maker...\n");
    account("{\"id\":9,\"username\":\"maker\",\"player_skinmaker\":1,\"skins\":[]}");
    CHECK(g_client.acct.skin_maker, "player_skinmaker 1: acct.skin_maker");
    CHECK(owns(PUMPIE) && !owns(DIORAMA) && !owns(BRAINIE) && !owns(SANDY) && !owns(CITIZENS), "season 6+, no creator, not blocked");
    account("{\"id\":9,\"username\":\"maker\",\"player_skinmaker\":0,\"skins\":[]}");
    CHECK(!g_client.acct.skin_maker && !owns(PUMPIE), "player_skinmaker 0: none");

    printf("Running a developer...\n");
    account("{\"id\":10,\"username\":\"dev\",\"dev\":true,\"skins\":[]}");
    CHECK(owns(ATARST) && owns(EMOJI_BEANIE) && owns(CITIZENS) && owns(PUMPIE) && owns(BRAINIE) && owns(8000) && owns(CUSTOM),
          "acc.isDev: every store skin and the customSkins");
    CHECK(!owns(60000), "an index past the store is no store skin");

    printf("Running enforcement off...\n");
    g_gs_config.enforce_skins = 0;
    account("{\"id\":0}");
    CHECK(owns(PUMPIE), "enforce_skins off: anything goes");

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
