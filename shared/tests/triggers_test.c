// WP-C: the Krunker v7.1.4 trigger runtime against v7 semantics (doc/v7/map-logic.md §3,
// shared/src/triggers_v7.c, triggers_conditions.c, triggers_actions.c, interact.c): events and
// their dispatch, conditions, actions, value types, delays, chains, single use per player, costs
// and methods, interface targeting, pickups, the join state and the per-round reset; then every
// trigger of the v7-format custom maps (and the v3 baseline Soul Sanctum) is fired once with a
// player and counted. Packets are captured like modes_test / match_test do.
// Run from the repository root: ./<build>/triggers_test
#include <shared.h>
#include <triggers_v7.h>
#include <account.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_SENT 4096
typedef struct {
    int uid, type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} Sent;
static Sent g_sent[MAX_SENT];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    if (g_sent_count >= MAX_SENT) return;
    Sent *s = &g_sent[g_sent_count++];
    s->uid = uid;
    s->type = type;
    s->length = length;
    if (payload && length) memcpy(s->payload, payload, length);
}

// the nth packet of a type (uid -99: any target), NULL none
static const Sent *sent(const int type, const int uid, const int nth) {
    int seen = 0;
    for (int i = 0; i < g_sent_count; i++) {
        if (g_sent[i].type != type || (uid != -99 && g_sent[i].uid != uid)) continue;
        if (seen++ == nth) return &g_sent[i];
    }
    return NULL;
}

static int count_sent(const int type) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) n += g_sent[i].type == type;
    return n;
}

static void clear_sent(void) { g_sent_count = 0; }

static int g_has_account;
static int test_account(const Game *game, const Player *player, AccountInfo *out) {
    (void) game;
    (void) player;
    out->has_account = (unsigned char) g_has_account;
    out->level = 12;
    return g_has_account;
}

static const char *g_custom_param;
static int g_custom_value, g_custom_calls;
static char g_custom_player[16];
static void test_custom_trigger(Game *game, const char *player_id, const char *param, const int value) {
    (void) game;
    g_custom_calls++;
    g_custom_value = value;
    g_custom_param = param;
    snprintf(g_custom_player, sizeof(g_custom_player), "%s", player_id);
}
static const TriggerScriptHooks g_hooks = {.on_custom_trigger = test_custom_trigger};

static int g_fake_server;
// every map JSON of the run stays alive until the end (games keep pointers into them)
static cJSON *g_jsons[64];
static int g_json_count;
static const cJSON *g_test_maps[64][1];

static const cJSON **keep_json(cJSON *json) {
    if (g_json_count >= 64) exit(1);
    g_jsons[g_json_count] = json;
    g_test_maps[g_json_count][0] = json;
    return g_test_maps[g_json_count++];
}

static Game *make_game(Game *game, const char *json) {
    memset(game, 0, sizeof(*game));
    cJSON *parsed = cJSON_Parse(json);
    if (!parsed) {
        printf("bad fixture json\n");
        exit(1);
    }
    game->server = &g_fake_server;
    game_configure(game, NULL, keep_json(parsed), 1, NULL, 0, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.account_info = test_account;
    game_init(game, 0, -1, 0);
    game->now = 10.0f;
    return game;
}

static Game *load_game(Game *game, const char *path) {
    memset(game, 0, sizeof(*game));
    cJSON *loaded = NULL;
    if (load_json_file(path, &loaded) || !loaded) return NULL;
    game->server = &g_fake_server;
    game_configure(game, NULL, keep_json(loaded), 1, NULL, 0, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game->hooks.account_info = test_account;
    game_init(game, 0, -1, 0);
    game->now = 10.0f;
    return game->ready ? game : NULL;
}

static Player *add_player(Game *game, const int uid) {
    Player *p = player_init(game);
    p->uid = uid;
    snprintf(p->name, sizeof(p->name), "P%d", uid);
    game_players_add(game, p);
    player_spawn(p);
    return p;
}

static void place(Player *p, const float x, const float y, const float z) {
    p->position = p->last_position = (vec3) {x, y, z};
    p->velocity = (vec3) {0};
}

static Object *obj(Game *game, const int uid) {
    return uid >= 0 && (size_t) uid < game->map->object_count ? game->map->objects[uid] : NULL;
}

static void fire(Game *game, Player *p, const int uid) {
    game_on_trigger(game, p, obj(game, uid), NULL);
}

static double cv(const Player *p, const char *key) { return player_custom_value_get(p, key); }

#define SPAWNS "\"spawns\":[[0,0,0]]"

// ---------------------------------------------------------------- core onTrigger rules

static void test_basic_actions(void) {
    Game g;
    // 0: trigger -> interface 5 (cube 1 hp 50 canDest, gate 2, bank 3), 4: a second trigger in 5 (self-exclusion)
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":0,\"in\":9,\"tin\":5,\"tas\":[{\"ta\":0,\"tc\":7}]},"
        "{\"i\":0,\"p\":[50,0,0],\"s\":[10,10,10],\"h\":50,\"in\":5},"
        "{\"i\":24,\"p\":[100,0,0],\"s\":[10,10,10],\"in\":5,\"cl\":1},"
        "{\"i\":31,\"p\":[150,0,0],\"s\":[10,10,10],\"in\":5},"
        "{\"i\":29,\"p\":[200,0,0],\"s\":[10,10,10],\"te\":12,\"tce\":3,\"in\":5,\"tas\":[{\"ta\":13,\"tc\":99}]}"
        "]}");
    Player *a = add_player(&g, 1);
    a->score = 0;
    Object *t = obj(&g, 0);
    TriggerActionData *act = &t->trig->actions[0];

    fire(&g, a, 0);
    CHECK(a->score == 7, "0 Give Player Score: %d", a->score);
    act->constant = 0;
    fire(&g, a, 0);
    CHECK(a->score == 8, "0 Give Player Score: (tc || 1)");

    // 6 Change Interface Health: canDest && !destroyed; onDamage only for tc < 0
    act->action = TRIGGER_ACTION_CHANGE_INTERFACE_HEALTH;
    act->constant = -20;
    clear_sent();
    fire(&g, a, 0);
    CHECK(obj(&g, 1)->health == 30.0f, "6 Change Interface Health -20: %.0f", obj(&g, 1)->health);
    act->constant = -40;
    fire(&g, a, 0);
    CHECK(obj(&g, 1)->destroyed && !obj(&g, 1)->active && sent(NET_PACKET_OBJECT_STATE, NET_TARGET_ALL, 0), "6: destroyed at 0 (\"do\")");

    // 11 Respawn Destructible Interface (no destroyedBy needed in v7)
    act->action = TRIGGER_ACTION_RESPAWN_DESTRUCTIBLE;
    obj(&g, 1)->destroyed_by = -1;
    fire(&g, a, 0);
    CHECK(!obj(&g, 1)->destroyed && obj(&g, 1)->active && obj(&g, 1)->health == 50.0f, "11 Respawn Destructible Interface");

    // 12 Toggle Destructible: destroy, then revive
    act->action = TRIGGER_ACTION_TOGGLE_DESTRUCTIBLE;
    fire(&g, a, 0);
    CHECK(obj(&g, 1)->destroyed, "12 Toggle Destructible: destroyed");
    fire(&g, a, 0);
    CHECK(!obj(&g, 1)->destroyed, "12 Toggle Destructible: revived");

    // 5 Toggle Interface Gate; 4 Destroy Interface closes gates and destroys destructibles
    act->action = TRIGGER_ACTION_TOGGLE_GATE;
    const int gate_was = obj(&g, 2)->active;
    fire(&g, a, 0);
    CHECK(obj(&g, 2)->active == !gate_was, "5 Toggle Interface Gate");
    act->action = TRIGGER_ACTION_DESTROY_INTERFACE;
    obj(&g, 2)->active = 1;
    fire(&g, a, 0);
    CHECK(!obj(&g, 2)->active && obj(&g, 1)->destroyed, "4 Destroy Interface: gate closed, cube destroyed");

    // 21 Reset Interface: revive, active, triggered cleared; the closed gate is re-sent (KN fix)
    act->action = TRIGGER_ACTION_RESET_INTERFACE;
    obj(&g, 4)->trig->triggered = 1;
    clear_sent();
    fire(&g, a, 0);
    CHECK(!obj(&g, 1)->destroyed && obj(&g, 2)->active && !obj(&g, 4)->trig->triggered, "21 Reset Interface");
    CHECK(sent(NET_PACKET_OBJECT_STATE, NET_TARGET_ALL, 0) != NULL, "21: \"ro\" / \"gte\" sent");

    // 8 / 48 deposit box
    act->action = TRIGGER_ACTION_CHANGE_DEPOSIT;
    act->constant = 30;
    fire(&g, a, 0);
    fire(&g, a, 0);
    CHECK(obj(&g, 3)->deposited == 60, "8 Change Deposit: %d", obj(&g, 3)->deposited);
    act->action = TRIGGER_ACTION_SET_DEPOSIT;
    act->constant = 5;
    fire(&g, a, 0);
    CHECK(obj(&g, 3)->deposited == 5, "48 Set Deposit");

    // 25-28 storage on trigger 4 (te 12 onStorageEquals, Event Value 3): Equals fires Set Player Score 99
    act->action = TRIGGER_ACTION_INCREASE_STORAGE;
    act->constant = 2;
    fire(&g, a, 0);
    CHECK(obj(&g, 4)->trig->storage == 2 && a->score != 99, "25 Increase Storage: 2 (no event)");
    act->constant = 1;
    fire(&g, a, 0);
    CHECK(obj(&g, 4)->trig->storage == 3 && a->score == 99, "25: storage == tce fires onStorageEquals");
    act->action = TRIGGER_ACTION_DECREASE_STORAGE;
    fire(&g, a, 0);
    CHECK(obj(&g, 4)->trig->storage == 2, "26 Decrease Storage");
    act->action = TRIGGER_ACTION_SET_STORAGE;
    act->constant = 40;
    fire(&g, a, 0);
    CHECK(obj(&g, 4)->trig->storage == 40, "27 Set Storage");
    act->action = TRIGGER_ACTION_RESET_STORAGE;
    fire(&g, a, 0);
    CHECK(obj(&g, 4)->trig->storage == 0, "28 Reset Storage (Equals target)");

    // self exclusion and interface rules: trigger 0 has in 9, a target with in 9 is not hit
    obj(&g, 1)->interface_id = 9;
    act->action = TRIGGER_ACTION_DESTROY_INTERFACE;
    fire(&g, a, 0);
    CHECK(!obj(&g, 1)->destroyed, "d.interface != j.interface: not a target");
    obj(&g, 1)->interface_id = 5;

    // 16 / 17 / 29 / 30 / 31 messages, popups, jump scare
    act->action = TRIGGER_ACTION_BROADCAST_MESSAGE;
    act->has_text = 1;
    snprintf(act->text, sizeof(act->text), "hello");
    clear_sent();
    fire(&g, a, 0);
    const Sent *chat = sent(NET_PACKET_CHAT, NET_TARGET_ALL, 0);
    CHECK(chat && !strcmp(((const NetChatPacket *) chat->payload)->text, "hello") && ((const NetChatPacket *) chat->payload)->kind == NET_CHAT_SERVER,
          "16 Broadcast Message (ch -1, null, text, 2)");
    act->action = TRIGGER_ACTION_SEND_MESSAGE;
    fire(&g, a, 0);
    CHECK(sent(NET_PACKET_CHAT, a->uid, 0) != NULL, "17 Send Message to the player");
    act->action = TRIGGER_ACTION_BROADCAST_POPUP;
    fire(&g, a, 0);
    CHECK(sent(NET_PACKET_POPUP, NET_TARGET_ALL, 0) != NULL, "29 Broadcast Popup");
    act->action = TRIGGER_ACTION_SEND_POPUP;
    fire(&g, a, 0);
    CHECK(sent(NET_PACKET_POPUP, a->uid, 0) != NULL, "30 Send Popup");
    act->action = TRIGGER_ACTION_JUMP_SCARE;
    fire(&g, a, 0);
    CHECK(sent(NET_PACKET_JUMP_SCARE, a->uid, 0) != NULL, "31 Jump Scare");
    act->has_text = 0;

    // 14 / 34 / 54 sounds: ef 2 = global 2D (uid -2), es 16 = scare_0 (v7 table)
    act->action = TRIGGER_ACTION_BROADCAST_SOUND;
    act->sound = 16;
    act->emit_from = 2;
    act->volume = act->rate = 1.0f;
    clear_sent();
    fire(&g, a, 0);
    const Sent *so = sent(NET_PACKET_SOUND, NET_TARGET_ALL, 0);
    CHECK(so && ((const NetSoundPacket *) so->payload)->uid == -2 && !strcmp(((const NetSoundPacket *) so->payload)->name, "scare_0"),
          "14 Broadcast Sound: global scare_0");
    act->emit_from = 1;
    act->action = TRIGGER_ACTION_SEND_SOUND;
    fire(&g, a, 0);
    so = sent(NET_PACKET_SOUND, a->uid, 0);
    CHECK(so && ((const NetSoundPacket *) so->payload)->uid == a->uid, "34 Send Sound: at the player, to the player");
    act->action = TRIGGER_ACTION_STOP_SOUND;
    fire(&g, a, 0);
    CHECK(sent(NET_PACKET_STOP_SOUND, NET_TARGET_ALL, 0) != NULL, "54 Stop Sound");

    // 55 / 56 GUI, 61 ambient
    act->action = TRIGGER_ACTION_GUI_ALL;
    act->constant = 3;
    act->constant2 = 2;
    clear_sent();
    fire(&g, a, 0);
    const Sent *gui = sent(NET_PACKET_GUI_TOGGLE, NET_TARGET_ALL, 0);
    CHECK(gui && ((const NetGuiTogglePacket *) gui->payload)->id == 3 && ((const NetGuiTogglePacket *) gui->payload)->status == 2, "55 GUI (all)");
    act->action = TRIGGER_ACTION_GUI_PLAYER;
    fire(&g, a, 0);
    CHECK(sent(NET_PACKET_GUI_TOGGLE, a->uid, 0) != NULL, "56 GUI (player)");
    act->action = TRIGGER_ACTION_CHANGE_AMBIENT;
    act->constant = 8; // the 9th key, "Tomb": 11
    fire(&g, a, 0);
    const Sent *amb = sent(NET_PACKET_AMBIENT_CHANGE, NET_TARGET_ALL, 0);
    CHECK(amb && ((const NetAmbientPacket *) amb->payload)->ambient == 11, "61 Change Ambient Sound: soundScapes value");

    // 19 Set Player Team clamps to 1..5; 18 swap
    act->action = TRIGGER_ACTION_SET_TEAM;
    act->constant = 9;
    fire(&g, a, 0);
    CHECK(a->team == 5, "19 Set Player Team clamp 5: %d", a->team);
    act->action = TRIGGER_ACTION_SWAP_TEAM;
    fire(&g, a, 0);
    CHECK(a->team == 2, "18 Swap Player Team (not 2 -> 2)");
    a->team = 0;

    // 3 Change Player Health, 23 refill knife, 15 clear loadout, 1 kill
    act->action = TRIGGER_ACTION_CHANGE_HEALTH; // changeHealth(h, null, -val): tc -10 hurts, +10 heals
    act->constant = -10;
    const float before = a->health;
    fire(&g, a, 0);
    CHECK(a->health == before - 10.0f, "3 Change Player Health tc -10: %.0f -> %.0f", before, a->health);
    act->action = TRIGGER_ACTION_REFILL_KNIFE;
    a->can_throw = 0;
    clear_sent();
    fire(&g, a, 0);
    CHECK(a->can_throw && sent(NET_PACKET_REFILL, a->uid, 0), "23 Refill Player Knife");
    act->action = TRIGGER_ACTION_CLEAR_LOADOUT;
    fire(&g, a, 0);
    CHECK(a->loadout_size == 1 && a->loadout[0] == WEAPON_ID_KNIFE, "15 Clear Player Loadout");
    act->action = TRIGGER_ACTION_CHANGE_PRIMARY;
    act->constant = 3;
    fire(&g, a, 0);
    CHECK(a->loadout_size == 2 && a->loadout[0] == 3, "50 Change Primary Weapon inserts at 0");
    act->action = TRIGGER_ACTION_CHANGE_SECONDARY;
    act->constant = WEAPON_ID_PISTOL;
    fire(&g, a, 0);
    CHECK(a->loadout_size == 3 && a->loadout[1] == WEAPON_ID_PISTOL, "51 Change Secondary Weapon after the primary");
    act->action = TRIGGER_ACTION_CHANGE_CLASS;
    act->constant = 2;
    fire(&g, a, 0);
    CHECK(a->class_index == 2 && a->health == (float) a->max_health, "47 Change Class");
    act->action = TRIGGER_ACTION_GIVE_EFFECT;
    act->constant = 0;
    fire(&g, a, 0);
    CHECK(a->effects[0] == 1, "64 Give Effect 0 (Boosted)");
    act->action = TRIGGER_ACTION_EXPLOSION;
    act->constant = 0;
    clear_sent();
    fire(&g, a, 0);
    CHECK(sent(NET_PACKET_EXPLOSION, NET_TARGET_ALL, 0) != NULL, "35 Explosion (tc || 100)");
    act->action = TRIGGER_ACTION_KILL_PLAYER;
    fire(&g, a, 0);
    CHECK(!a->active, "1 Kill Player");
    printf("basic actions done\n");
}

static void test_values_and_conditions(void) {
    Game g;
    // trigger 0 (te 0): actions edited below; trigger 1 (in 4, te 24 onCustomValEquals tce 5,
    // te set per case): target of 43 / 44
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":0,\"tin\":4,\"tas\":["
        "  {\"ta\":37,\"tct\":\"Coins!\",\"tc\":3},"
        "  {\"ta\":0,\"tc\":1,\"tce\":2,\"tcc\":\"coins\"},"
        "  {\"ta\":52,\"tct\":\"Round\",\"tc\":2},"
        "  {\"ta\":0,\"tc\":1,\"cd\":1,\"cv0\":\"coins\",\"cv1\":3,\"cv2\":3}]},"
        "{\"i\":29,\"p\":[50,0,0],\"s\":[10,10,10],\"te\":24,\"tce\":5,\"in\":4,\"tas\":[{\"ta\":13,\"tc\":500}]}"
        "]}");
    Player *a = add_player(&g, 1);
    a->score = 0;
    triggers_set_script_hooks(&g, &g_hooks);
    clear_sent();
    fire(&g, a, 0);
    // 37: key "Coins!" reduced to [a-z0-9_] "Coins" (case kept), so the value-type key "coins" misses
    CHECK(cv(a, "Coins") == 3.0, "37 Set Player Value (key [a-z0-9_]): %g", cv(a, "Coins"));
    CHECK(sent(NET_PACKET_CUSTOM_VALUE, a->uid, 0) != NULL, "37: \"cv\" to the player");
    CHECK(trigger_global_value_get(&g, "Round") == 2.0 && sent(NET_PACKET_GLOBAL_VALUE, NET_TARGET_ALL, 0), "52 Set Global Value + \"gv\"");
    CHECK(a->score == 0, "value type 2 with a missing key skips; condition 1 (coins >= 3) false: score %d", a->score);

    TriggerActionData *acts = obj(&g, 0)->trig->actions;
    snprintf(acts[0].text, sizeof(acts[0].text), "coins");
    fire(&g, a, 0); // coins = 3, then value type 2 reads 3, then condition coins >= 3 passes
    CHECK(cv(a, "coins") == 3.0 && a->score == 3 + 1, "value type 2 (player value) + condition 1: score %d", a->score);
    acts[0].action = TRIGGER_ACTION_CHANGE_PLAYER_VALUE;
    acts[0].constant = -3;
    a->score = 0;
    fire(&g, a, 0); // coins 0: value type 2 reads 0 -> undefined (skip); condition false
    CHECK(cv(a, "coins") == 0.0 && a->score == 0, "38 Inc/Dec Player Value; a stored 0 skips (parseInt || undefined)");

    // value type 2 with a built-in player value (score) and type 3 (global)
    acts[1].value_type = 2;
    snprintf(acts[1].value_key, sizeof(acts[1].value_key), "score");
    a->score = 4;
    fire(&g, a, 0);
    CHECK(a->score == 8, "value type 2 \"score\" (config.playerVals): %d", a->score);
    acts[1].value_type = 3;
    snprintf(acts[1].value_key, sizeof(acts[1].value_key), "Round");
    a->score = 0;
    fire(&g, a, 0);
    CHECK(a->score == 2, "value type 3 (global value): %d", a->score);
    acts[1].value_type = 1; // trigger value of the trigger itself (none yet): skip
    a->score = 0;
    fire(&g, a, 0);
    CHECK(a->score == 0, "value type 1 without the key skips");

    // 45 / 46 trigger values (self), then value type 1 reads them
    acts[0].action = TRIGGER_ACTION_SET_TRIGGER_VALUE;
    acts[0].constant = 6;
    acts[0].has_text = 1;
    snprintf(acts[0].text, sizeof(acts[0].text), "Round");
    acts[1].value_type = 1;
    snprintf(acts[1].value_key, sizeof(acts[1].value_key), "Round");
    a->score = 0;
    fire(&g, a, 0);
    const TriggerValue *own = trigger_values_find(trigger_object_values(&g, obj(&g, 0)), "Round");
    CHECK(own && own->value == 6.0 && a->score == 6, "45 Set Trigger Value (Self) + value type 1: %d", a->score);
    acts[0].action = TRIGGER_ACTION_CHANGE_TRIGGER_VALUE;
    fire(&g, a, 0);
    CHECK(own->value == 12.0, "46 Inc/Dec Trigger Value (Self)");

    // 43 / 44 interface trigger values and onCustomValEquals / Greater / Lesser on the target
    // (the other actions of the list do nothing here: 67 Play Video Ad)
    acts[1].action = acts[2].action = acts[3].action = TRIGGER_ACTION_VIDEO_AD;
    acts[0].action = TRIGGER_ACTION_SET_INTERFACE_VALUE;
    acts[0].constant = 5;
    a->score = 0;
    fire(&g, a, 0);
    CHECK(a->score == 500, "43 Set Interface Trigger Value == tce 5 fires onCustomValEquals (%d)", a->score);
    obj(&g, 1)->trig->event = TRIGGER_EVENT_ON_CUSTOM_VAL_GREATER;
    acts[0].action = TRIGGER_ACTION_CHANGE_INTERFACE_VALUE;
    acts[0].constant = 1;
    a->score = 0;
    fire(&g, a, 0);
    CHECK(a->score == 500, "44 Inc/Dec Interface Trigger Value 6 > 5: onCustomValGreater");
    obj(&g, 1)->trig->event = TRIGGER_EVENT_ON_CUSTOM_VAL_LESSER;
    acts[0].constant = -4;
    a->score = 0;
    fire(&g, a, 0);
    CHECK(a->score == 500, "44: 2 < 5 onCustomValLesser");
    a->score = 0;
    acts[0].constant = 1; // 3 < 5: Lesser again, but Equals / Greater targets would not fire
    obj(&g, 1)->trig->event = TRIGGER_EVENT_ON_CUSTOM_VAL_EQUALS;
    fire(&g, a, 0);
    CHECK(a->score == 0, "44: 3 != 5 no onCustomValEquals");

    // conditions on a fresh game: [Give 1, Give 1 with condition]
    {
        Game c;
        make_game(&c, "{\"name\":\"c\"," SPAWNS ",\"objects\":["
            "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":0,\"tc\":1},{\"ta\":0,\"tc\":1,\"cd\":2}]}]}");
        Player *p = add_player(&c, 1);
        TriggerActionData *cond = &obj(&c, 0)->trig->actions[1];
        const struct { int cd; int account; int expect; const char *what; } cases[] = {
            {2, 0, 2, "condition 2 Current Class 0 == cv0 (default 0)"},
            {6, 0, 1, "condition 6 Player has Account: guest"},
            {6, 1, 2, "condition 6 Player has Account"},
            {7, 1, 1, "condition 7 Premium: false (no premium)"},
            {10, 1, 1, "condition 10 Player Level == 0 with level 12: false"},
            {4, 1, 1, "condition 4 Player Count == 0 (1 active): false"},
            {5, 1, 2, "condition 5 Global Value \"\" == 0: true"},
            {3, 1, 2, "condition 3 Trigger Value \"\" == 0: true"},
            {99, 1, 2, "an unknown condition is None"},
        };
        p->class_index = 0;
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            cond->condition = cases[i].cd;
            g_has_account = cases[i].account;
            p->has_account = 0;
            p->score = 0;
            fire(&c, p, 0);
            CHECK(p->score == cases[i].expect, "%s (score %d)", cases[i].what, p->score);
        }
        game_on_trigger(&c, NULL, obj(&c, 0), NULL);
        CHECK(1, "no player: conditions 1-11 false, no crash");
    }

    // 58 Custom Action -> KrunkScript onCustomTrigger(player id, tct, tc)
    acts[0].action = TRIGGER_ACTION_CUSTOM;
    snprintf(acts[0].text, sizeof(acts[0].text), "door");
    acts[0].constant = 4;
    g_custom_calls = 0;
    fire(&g, a, 0);
    CHECK(g_custom_calls == 1 && !strcmp(g_custom_param, "door") && g_custom_value == 4 && !strcmp(g_custom_player, "1"), "58 Custom Action");
    game_on_trigger(&g, NULL, obj(&g, 0), NULL);
    CHECK(!strcmp(g_custom_player, "NONE"), "58 without a player: \"NONE\"");

    // the round reset: globals, object custVals cleared; player custVals kept
    trigger_round_reset(&g);
    CHECK(trigger_global_value_get(&g, "Round") == 0.0 && !trigger_object_values(&g, obj(&g, 0)), "round reset: globalVals / custVals cleared");
    CHECK(cv(a, "Coins") == 3.0 && a->custom_values.count >= 2, "round reset: player custVals kept");

    // the join state: a "gbl" reset first, then every value
    trigger_global_value_set(&g, "k", 7.0);
    clear_sent();
    trigger_send_join_state(&g, a->uid);
    const Sent *first = sent(NET_PACKET_GLOBAL_VALUE, a->uid, 0), *second = sent(NET_PACKET_GLOBAL_VALUE, a->uid, 1);
    CHECK(first && (((const NetValuePacket *) first->payload)->flags & NET_VALUE_RESET) && second &&
          ((const NetValuePacket *) second->payload)->value == 7.0, "join: gbl list");

    // custValLimit: 31 keys at most
    for (int i = 0; i < 40; i++) {
        char key[16];
        snprintf(key, sizeof(key), "k%d", i);
        trigger_global_value_set(&g, key, 1.0);
    }
    CHECK(trigger_global_values(&g)->count == TRIGGER_VALUE_SLOTS, "custValLimit: %d keys", trigger_global_values(&g)->count);
    triggers_set_script_hooks(&g, NULL);
    printf("values / conditions done\n");
}

static void test_flow(void) {
    Game g;
    // 0: two actions, the second delayed 1.5 s with a condition; 1 / 2: storage ping-pong chain; 3: sup + cha
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":0,\"tc\":1},{\"ta\":0,\"tc\":10,\"tad\":1.5,\"cd\":1,\"cv0\":\"gate\",\"cv1\":0,\"cv2\":0}]},"
        "{\"i\":29,\"p\":[50,0,0],\"s\":[10,10,10],\"te\":13,\"in\":1,\"tin\":2,\"tas\":[{\"ta\":25,\"tc\":1},{\"ta\":0,\"tc\":1}]},"
        "{\"i\":29,\"p\":[100,0,0],\"s\":[10,10,10],\"te\":13,\"in\":2,\"tin\":1,\"tas\":[{\"ta\":25,\"tc\":1}]},"
        "{\"i\":29,\"p\":[150,0,0],\"s\":[10,10,10],\"te\":0,\"sup\":1,\"tas\":[{\"ta\":0,\"tc\":1}]},"
        "{\"i\":29,\"p\":[200,0,0],\"s\":[10,10,10],\"te\":6,\"tas\":[{\"ta\":0,\"tc\":1}]},"
        "{\"i\":29,\"p\":[250,0,0],\"s\":[10,10,10],\"te\":6,\"tce\":3,\"tas\":[{\"ta\":0,\"tc\":1}]}"
        "]}");
    Player *a = add_player(&g, 1), *b = add_player(&g, 2);
    a->score = b->score = 0;

    // delays: the condition (gate == 0) passes when queued and is not checked again
    fire(&g, a, 0);
    CHECK(a->score == 1, "the immediate action ran");
    player_custom_value_set(&g, a, "gate", 9.0);
    trigger_queue_update(&g, 1.0f);
    CHECK(a->score == 1, "delayed: not yet after 1 s");
    trigger_queue_update(&g, 0.6f);
    CHECK(a->score == 11, "delayed action after 1.5 s, condition not re-checked (%d)", a->score);

    // chain limit 30: storage ping-pong (onStorageGreater 0 each), each onTrigger costs one
    a->score = 0;
    fire(&g, a, 1);
    CHECK(a->score == TRIGGER_LIMIT / 2, "chain limit: trigger 1 ran %d times (30 calls across the chain)", a->score);

    // sup: once per player per round
    a->score = b->score = 0;
    fire(&g, a, 3);
    fire(&g, a, 3);
    fire(&g, b, 3);
    CHECK(a->score == 1 && b->score == 1, "sup: once per player (%d, %d)", a->score, b->score);
    trigger_round_reset(&g);
    fire(&g, a, 3);
    CHECK(a->score == 2, "sup: uses cleared each round");

    // onTimer: tce 0 -> every 1 s (v7 (constE || 1)), tce 3 -> every 3 s
    a->score = 0;
    for (int i = 0; i < 60; i++) trigger_queue_update(&g, 0.1f); // 6 s
    // the timer objects fire with no player: Give Player Score needs one, so count the calls via triggered
    CHECK(obj(&g, 4)->trig->triggered && obj(&g, 5)->trig->triggered, "onTimer fired (tce 0 and 3)");
    CHECK(fabsf(obj(&g, 4)->trig->timer - 1000.0f) < 101.0f, "onTimer tce 0: 1 s period (timer %.0f)", obj(&g, 4)->trig->timer);

    // costs: tam 1 (cost score) 10
    TriggerData *t = obj(&g, 3)->trig;
    t->single_use_player = 0;
    t->action_method = 1;
    t->action_cost = 10;
    a->score = 5;
    fire(&g, a, 3);
    CHECK(a->score == 5, "tam 1: not enough score");
    a->score = 15;
    clear_sent();
    fire(&g, a, 3);
    const Sent *medal = sent(NET_PACKET_MEDAL, a->uid, 0);
    CHECK(a->score == 6 && medal && !strcmp(((const NetMedalPacket *) medal->payload)->text, "Purchased"), "tam 1: pays 10, then +1 (%d)", a->score);
    t->action_method = 2; // require score: kept
    a->score = 15;
    fire(&g, a, 3);
    CHECK(a->score == 16, "tam 2: required, not paid");
    t->action_method = 3; // require kills
    a->kills = 3;
    a->score = 0;
    fire(&g, a, 3);
    CHECK(a->score == 0, "tam 3: kills 3 < 10");
    t->action_method = 4; // account
    t->action_cost = 0;
    g_has_account = 0;
    a->has_account = 0;
    fire(&g, a, 3);
    CHECK(a->score == 0, "tam 4: guest");
    g_has_account = 1;
    fire(&g, a, 3);
    CHECK(a->score == 1, "tam 4: account");
    t->action_method = 0; // KR: no wallet hook -> unaffordable
    t->action_cost = 5;
    fire(&g, a, 3);
    CHECK(a->score == 1, "tam 0 KR cost without a wallet: nothing");

    // chance 100: randInt(0, 100) <= 100 always
    t->action_cost = 0;
    t->chance = 100;
    fire(&g, a, 3);
    CHECK(a->score == 2, "cha 100 always fires");
    t->chance = 0;

    // single use
    t->single_use = 1;
    t->triggered = 0;
    fire(&g, a, 3);
    fire(&g, a, 3);
    CHECK(a->score == 3, "su: once");
    printf("flow done\n");
}

static void test_events(void) {
    Game g;
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":17,\"tas\":[{\"ta\":37,\"tct\":\"spawn\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[50,0,0],\"s\":[10,10,10],\"te\":18,\"tas\":[{\"ta\":37,\"tct\":\"kill\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[100,0,0],\"s\":[10,10,10],\"te\":19,\"tas\":[{\"ta\":37,\"tct\":\"die\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[150,0,0],\"s\":[10,10,10],\"te\":28,\"tas\":[{\"ta\":38,\"tct\":\"hurt\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[200,0,0],\"s\":[10,10,10],\"te\":26,\"tas\":[{\"ta\":52,\"tct\":\"started\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[250,0,0],\"s\":[10,10,10],\"te\":21,\"tas\":[{\"ta\":52,\"tct\":\"nuke\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[300,0,0],\"s\":[10,10,10],\"te\":23,\"tas\":[{\"ta\":52,\"tct\":\"exit\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[350,0,0],\"s\":[10,10,10],\"te\":16,\"tas\":[{\"ta\":52,\"tct\":\"key\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[400,0,0],\"s\":[10,10,10],\"te\":27,\"tas\":[{\"ta\":52,\"tct\":\"obj\",\"tc\":1}]},"
        "{\"i\":29,\"p\":[450,0,0],\"s\":[10,10,10],\"te\":16,\"tac\":50,\"tas\":[{\"ta\":52,\"tct\":\"kr\",\"tc\":1}]}"
        "]}");
    CHECK(trigger_global_value_get(&g, "started") == 1.0, "26 onGameStart at game_init (game_start_round)");
    Player *a = add_player(&g, 1), *b = add_player(&g, 2);
    CHECK(cv(a, "spawn") == 1.0 && cv(b, "spawn") == 1.0, "17 onPlayerSpawn at the authority's spawn (KN decision)");
    a->god_mode = b->god_mode = 0;
    player_change_health(b, a, 10.0f, 0);
    CHECK(cv(b, "hurt") == 1.0, "28 onPlayerDamage (victim)");
    player_kill(b, a, NULL, 0);
    CHECK(cv(b, "die") == 1.0 && cv(a, "kill") == 1.0, "19 onPlayerDie (victim) / 18 onPlayerKill (killer)");
    game_start_nuke(&g, a);
    CHECK(trigger_global_value_get(&g, "nuke") == 1.0, "21 onNukeStart");
    trigger_object_event(&g, obj(&g, 6), TRIGGER_EVENT_ON_EXIT_OBJECT, a);
    CHECK(trigger_global_value_get(&g, "exit") == 1.0, "23 onExitObject (objectTrigger)");
    trigger_multi_event(&g, TRIGGER_EVENT_ON_OBJECTIVE_SWITCH, NULL);
    CHECK(trigger_global_value_get(&g, "obj") == 1.0, "27 onObjectiveSwitch (multiObjectTrigger)");
    obj(&g, 8)->active = 0;
    trigger_multi_event(&g, TRIGGER_EVENT_ON_OBJECTIVE_SWITCH, NULL);
    CHECK(trigger_global_value_get(&g, "obj") == 1.0, "inactive objects do not take dispatched events");

    // 16 onInteractKey: "int" 7, uid within radius 7, the interactTimer
    place(a, 350.0f, 0.0f, 0.0f);
    g.now += 1.0f;
    game_player_interact_v7(&g, a, GAME_INTERACT_KEY, 7, 0);
    CHECK(trigger_global_value_get(&g, "key") == 1.0, "16 onInteractKey via int 7");
    game_player_interact_v7(&g, a, GAME_INTERACT_KEY, 7, 0);
    CHECK(trigger_global_value_get(&g, "key") == 1.0, "interactTimer 100 ms");
    place(a, 380.0f, 0.0f, 0.0f);
    g.now += 1.0f;
    game_player_interact_v7(&g, a, GAME_INTERACT_KEY, 7, 0);
    CHECK(trigger_global_value_get(&g, "key") == 1.0, "out of range: nothing");
    GameInteractScan scan;
    place(a, 352.0f, 0.0f, 0.0f);
    game_interact_scan(&g, a, &scan);
    CHECK(scan.type == GAME_INTERACT_KEY && scan.index == 7 && scan.last_type == GAME_INTERACT_KEY && scan.last_uid == 7, "prompt: interact key 7");
    // a KR-cost onInteractKey: no confirmation without enough KR (no wallet), nothing fires
    place(a, 450.0f, 0.0f, 0.0f);
    g.now += 1.0f;
    clear_sent();
    game_player_interact_v7(&g, a, GAME_INTERACT_KEY, 9, 0);
    CHECK(!sent(NET_PACKET_CONFIRM_INTERACT, -99, 0) && trigger_global_value_get(&g, "kr") == 0.0, "KR cost without KR: no cnfm, no trigger");
    printf("events done\n");
}

// ---------------------------------------------------------------- Reward Junk (doc/v10/junk.md)

// the scripted wallet (GameHooks.junk_credit): what the game asks kn_brain to credit
static int g_junk_calls;
static int g_junk_uid;
static int64_t g_junk_hundredths;
static char g_junk_reason[16];
static void test_junk_credit(Game *game, const Player *player, const int64_t hundredths, const char *reason) {
    (void) game;
    g_junk_calls++;
    g_junk_uid = player->uid;
    g_junk_hundredths = hundredths;
    snprintf(g_junk_reason, sizeof(g_junk_reason), "%s", reason ? reason : "");
}

// the pickup fires for p with the map's text (tct) as its amount
static void junk_pickup(Game *game, Player *p, const char *text) {
    TriggerActionData *act = &obj(game, 0)->trig->actions[0];
    snprintf(act->text, sizeof(act->text), "%s", text);
    act->has_text = 1;
    g_junk_calls = 0;
    clear_sent();
    fire(game, p, 0);
}

static void test_reward_junk(void) {
    Game g;
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":59,\"tct\":\"0.01\"}]}"
        "]}");
    g.hooks.junk_credit = test_junk_credit;
    Player *a = add_player(&g, 1);
    g_has_account = 1;
    junk_pickup(&g, a, "0.01");
    CHECK(g_junk_calls == 1 && g_junk_uid == 1 && g_junk_hundredths == 1 && !strcmp(g_junk_reason, "trigger"),
          "Reward Junk 0.01, an account player: credited 1 hundredth (%d calls)", g_junk_calls);
    CHECK(!count_sent(NET_PACKET_CHAT), "Reward Junk: no chat line from the game (the server prints it once the brain accepts)");
    junk_pickup(&g, a, "2.5");
    CHECK(g_junk_calls == 1 && g_junk_hundredths == 250, "Reward Junk 2.5: 250 hundredths (%lld)", (long long) g_junk_hundredths);
    junk_pickup(&g, a, "99.99");
    CHECK(g_junk_calls == 1 && g_junk_hundredths == 9999, "Reward Junk 99.99 (below 100): 9999 hundredths");
    junk_pickup(&g, a, "100");
    CHECK(g_junk_calls == 0, "Reward Junk 100: not below 100, nothing");
    junk_pickup(&g, a, "0");
    CHECK(g_junk_calls == 0, "Reward Junk 0: nothing");
    junk_pickup(&g, a, "-1");
    CHECK(g_junk_calls == 0, "Reward Junk -1: not credited (the wallet never goes below 0)");
    junk_pickup(&g, a, "abc");
    CHECK(g_junk_calls == 0, "Reward Junk 'abc' (parseFloat NaN): nothing");
    junk_pickup(&g, a, "0x10");
    CHECK(g_junk_calls == 0, "Reward Junk '0x10' (parseFloat 0): nothing");

    g_has_account = 0;
    junk_pickup(&g, a, "0.01");
    CHECK(g_junk_calls == 0, "Reward Junk, a guest (no account): nothing");
    g_has_account = 1;
    a->is_bot = (unsigned char) 1;
    junk_pickup(&g, a, "0.01");
    CHECK(g_junk_calls == 0, "Reward Junk, a bot: nothing");
    a->is_bot = (unsigned char) 0;
    g.is_sandbox = 1;
    junk_pickup(&g, a, "0.01");
    CHECK(g_junk_calls == 0, "Reward Junk in the sandbox (!isSandbox): nothing");
    g.is_sandbox = 0;
    junk_pickup(&g, a, "0.01");
    CHECK(g_junk_calls == 1, "Reward Junk back in a public game: credited again");
}

static void test_pickups(void) {
    Game g;
    // pickup 0: weapon 3 (pickupRep), pickup 1: weapon 4 with wr (no swap), price 20 (md 0)
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":26,\"p\":[0,0,0],\"s\":[10,10,10],\"w\":3},"
        "{\"i\":26,\"p\":[100,0,0],\"s\":[10,10,10],\"w\":4,\"wr\":1,\"sp\":20}"
        "]}");
    Player *a = add_player(&g, 1);
    const int primary = a->loadout[0];
    place(a, 0.0f, 0.0f, 0.0f);
    GameInteractScan scan;
    game_interact_scan(&g, a, &scan);
    CHECK(scan.type == GAME_INTERACT_PICKUP && scan.index == 0, "pickup prompt (type 6)");
    g.now += 1.0f;
    clear_sent();
    game_player_interact_v7(&g, a, GAME_INTERACT_PICKUP, 0, 0);
    GamePickup k;
    game_pickup_state(&g, 0, &k);
    CHECK(a->loadout[0] == 3 && k.weapon == primary, "pickupRep: weapon 3 taken, the old primary %d left in the pickup (%d)", primary, k.weapon);
    CHECK(sent(NET_PACKET_LOADOUT, NET_TARGET_ALL, 0) && sent(NET_PACKET_PICKUP, NET_TARGET_ALL, 0), "inat: loadout + pickup");
    CHECK(a->last_picked[3] == 0, "a swap from a pickup that held another weapon: lastPicked of the new weapon not set");

    place(a, 100.0f, 0.0f, 0.0f);
    a->score = 5;
    g.now += 1.0f;
    game_player_interact_v7(&g, a, GAME_INTERACT_PICKUP, 1, 0);
    CHECK(a->loadout[0] == 3, "price 20 with score 5: no pickup");
    a->score = 25;
    g.now += 1.0f;
    game_player_interact_v7(&g, a, GAME_INTERACT_PICKUP, 1, 0);
    game_pickup_state(&g, 1, &k);
    CHECK(a->loadout[0] == 4 && a->score == 5 && k.weapon == 4, "wr: no swap, the pickup keeps weapon 4; paid 20 (%d)", a->score);

    // mode.unrestrictedWeapons: a held weapon's ammo for half the price, full ammo, the pickup stays
    g.mode->config.unrestricted_weapons = 1;
    a->ammo[0] = 1;
    a->score = 30;
    g.now += 1.0f;
    game_player_interact_v7(&g, a, GAME_INTERACT_PICKUP, 1, 0);
    game_pickup_state(&g, 1, &k);
    CHECK(a->loadout[0] == 4 && a->ammo[0] > 1 && a->score == 20 && k.weapon == 4, "unrestricted: ammo bought for 10 (%d, ammo %u)", a->score, a->ammo[0]);
    g.mode->config.unrestricted_weapons = 0;

    // drop: only weapons taken from a pickupRep pickup that is empty now go back
    Game h;
    make_game(&h, "{\"name\":\"t\"," SPAWNS ",\"objects\":[{\"i\":26,\"p\":[0,0,0],\"s\":[10,10,10],\"w\":3}]}");
    Player *b = add_player(&h, 1);
    int knife = WEAPON_ID_KNIFE;
    mode_set_player_loadout(b, &knife, 1, 0); // no primary: the pickup is added, the pickup empties
    place(b, 0.0f, 0.0f, 0.0f);
    h.now += 1.0f;
    game_player_interact_v7(&h, b, GAME_INTERACT_PICKUP, 0, 0);
    game_pickup_state(&h, 0, &k);
    CHECK(b->loadout[0] == 3 && k.weapon == -1 && b->last_picked[3] == 1, "added to the loadout; the pickupRep pickup is empty");
    b->loadout_index = 0;
    game_player_drop_weapon(&h, b);
    game_pickup_state(&h, 0, &k);
    CHECK(k.weapon == 3 && b->loadout_size == 1, "drpw: back into its pickup (%d), loadout %d", k.weapon, b->loadout_size);
    printf("pickups done\n");
}

static void test_legacy(void) {
    Game g;
    // a v3 trigger: single-action keys, onTimer with tce 0 (v7: every 1 s), Broadcast Popup
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":6,\"ta\":29,\"tct\":\"tick\"},"
        "{\"i\":29,\"p\":[50,0,0],\"s\":[10,10,10],\"te\":0,\"ta\":0,\"tc\":4,\"tce\":7}"
        "]}");
    Player *a = add_player(&g, 1);
    clear_sent();
    trigger_queue_update(&g, 1.05f);
    CHECK(sent(NET_PACKET_POPUP, NET_TARGET_ALL, 0) != NULL, "v3 onTimer tce 0 fires after 1 s");
    a->score = 0;
    fire(&g, a, 1);
    CHECK(a->score == 4, "v3 wrap: tce copied to the value type is not kept (KN fix): score %d", a->score);
    printf("legacy done\n");
}

// ---------------------------------------------------------------- real maps

static void test_map(const char *path, const int expect_triggers) {
    Game g;
    if (!load_game(&g, path)) {
        CHECK(0, "%s: load", path);
        return;
    }
    Player *a = add_player(&g, 1);
    add_player(&g, 2);
    int triggers = 0, actions = 0, unknown = 0;
    int per_action[TRIGGER_ACTION_COUNT] = {0};
    for (size_t i = 0; i < g.map->object_count; i++) {
        const Object *o = g.map->objects[i];
        if (!o->trig) continue;
        triggers++;
        for (int k = 0; k < o->trig->action_count; k++) {
            const int ta = o->trig->actions[k].action;
            if (ta < 0 || ta >= TRIGGER_ACTION_COUNT) unknown++;
            else per_action[ta]++;
            actions++;
        }
    }
    CHECK(triggers >= expect_triggers, "%s: %d triggers (>= %d)", path, triggers, expect_triggers);
    CHECK(!unknown, "%s: %d actions outside 0..71", path, unknown);

    // fire every trigger once with a player and run the queue out (delays <= 60 s)
    int fired = 0;
    for (size_t i = 0; i < g.map->object_count; i++) {
        Object *o = g.map->objects[i];
        if (!o->trig) continue;
        if (!a->active) player_spawn(a);
        a->god_mode = 1;
        a->score = 100000;
        a->kills = 1000;
        const int was = o->trig->triggered;
        o->trig->single_use = 0;
        o->trig->triggered = 0;
        clear_sent();
        game_on_trigger(&g, a, o, NULL);
        fired += o->trig->triggered;
        (void) was;
    }
    for (int s = 0; s < 61 * 10; s++) trigger_queue_update(&g, 0.1f);
    trigger_round_reset(&g);
    printf("%-48s %3d triggers, %3d actions, %3d fired (KR-cost ones need a wallet)\n", path, triggers, actions, fired);
}

// ---------------------------------------------------------------- editor WP4 output

// The triggers v7.1.4's editor wrote after the logic UI edits of tools/editor/logic_cases.json
// (doc/ref/krunker-v7.1.4/editor/logic/, which the native editor reproduces: editor_serialize_test,
// tools/editor/logic_parity.py) load into the runtime with every action field as edited.
static void test_editor_goldens(void) {
    static const char *const cases[] = {"trigger-actions", "trigger-value-type-reset", "trigger-default"};
    for (size_t c = 0; c < sizeof(cases) / sizeof(cases[0]); c++) {
        char path[256];
        snprintf(path, sizeof(path), "doc/ref/krunker-v7.1.4/editor/logic/%s.json", cases[c]);
        cJSON *golden = NULL;
        if (load_json_file(path, &golden) || !golden) {
            CHECK(0, "%s: load", path);
            continue;
        }
        cJSON *map = cJSON_Parse("{\"name\":\"t\"," SPAWNS ",\"objects\":[]}");
        cJSON_AddItemToArray(cJSON_GetObjectItemCaseSensitive(map, "objects"), cJSON_Duplicate(golden, 1));
        char *text = cJSON_PrintUnformatted(map);
        cJSON_Delete(map);
        Game g;
        make_game(&g, text);
        free(text);
        const Object *o = obj(&g, 0);
        const TriggerData *t = o ? o->trig : NULL;
        CHECK(t != NULL, "%s: a trigger", cases[c]);
        if (!t) {
            cJSON_Delete(golden);
            continue;
        }
        const cJSON *tas = cJSON_GetObjectItemCaseSensitive(golden, "tas");
#define GI(obj, k) (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(obj, k)) ? (int) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(obj, k)) : 0)
        CHECK(t->event == GI(golden, "te") && t->constant_event == GI(golden, "tce") && t->chance == GI(golden, "cha"),
              "%s: te %d tce %d cha %d", cases[c], t->event, t->constant_event, t->chance);
        CHECK(t->action_method == GI(golden, "tam") && t->action_cost == GI(golden, "tac"), "%s: tam %d tac %d", cases[c], t->action_method, t->action_cost);
        CHECK(t->action_count == cJSON_GetArraySize(tas), "%s: %d actions (%d)", cases[c], t->action_count, cJSON_GetArraySize(tas));
        for (int i = 0; i < t->action_count && i < cJSON_GetArraySize(tas); i++) {
            const cJSON *a = cJSON_GetArrayItem(tas, i);
            const TriggerActionData *d = &t->actions[i];
            const char *tct = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "tct"));
            const char *tcc = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(a, "tcc"));
            CHECK(d->action == GI(a, "ta") && d->constant == GI(a, "tc") && d->constant2 == GI(a, "tc2") && d->value_type == GI(a, "tce"),
                  "%s action %d: ta %d tc %d tc2 %d tce %d", cases[c], i, d->action, d->constant, d->constant2, d->value_type);
            CHECK(fabsf(d->delay - (float) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(a, "tad"))) < 1e-4f || (!cJSON_GetObjectItemCaseSensitive(a, "tad") && d->delay == 0),
                  "%s action %d: tad %g", cases[c], i, (double) d->delay);
            CHECK(!tct || (d->has_text && !strcmp(d->text, tct)), "%s action %d: tct \"%s\"", cases[c], i, d->text);
            CHECK(!strcmp(d->value_key, tcc ? tcc : ""), "%s action %d: tcc \"%s\"", cases[c], i, d->value_key);
            CHECK(d->condition == GI(a, "cd"), "%s action %d: cd %d", cases[c], i, d->condition);
            for (int k = 0; k < 3; k++) {
                char key[8];
                snprintf(key, sizeof(key), "cv%d", k);
                CHECK((d->condition_values[k] != NULL) == (cJSON_GetObjectItemCaseSensitive(a, key) != NULL), "%s action %d: %s", cases[c], i, key);
            }
            if (d->action == 14) {
                CHECK(d->sound == GI(a, "es") && fabsf(d->volume - 0.5f) < 1e-4f && fabsf(d->rate - 2.0f) < 1e-4f && d->emit_from == GI(a, "ef"),
                      "%s action %d: sound %d vol %g rate %g from %d", cases[c], i, d->sound, (double) d->volume, (double) d->rate, d->emit_from);
            }
        }
#undef GI
        if (!strcmp(cases[c], "trigger-actions")) {
            // fire it: Require Score (tam 2, cost 50) passes at 100; action 0 gives 25 after 1.5 s;
            // action 2's value type (player value "kills") is missing, so it skips; action 3's
            // condition (Spectating players > 2) is false
            Player *a = add_player(&g, 1);
            a->score = 100;
            obj(&g, 0)->trig->chance = 0; // (40 %: drawn per fire)
            clear_sent();
            fire(&g, a, 0);
            CHECK(a->score == 100, "editor trigger: action 0 waits for its delay (%d)", a->score);
            for (int s = 0; s < 16; s++) trigger_queue_update(&g, 0.1f);
            CHECK(a->score == 125, "editor trigger: Give Player Score 25 after 1.5 s (%d)", a->score);
            CHECK(a->class_index != 5, "editor trigger: Change Class held back by its Player Count condition");
        }
        cJSON_Delete(golden);
    }
    printf("editor goldens done\n");
}

// ---------------------------------------------------------------- WP6 account backends

static char g_saved_key[64], g_saved_value[64];
static int g_saved_type, g_saves, g_loads, g_rewards, g_reward_amount;
static void hook_save(Game *game, const Player *p, const char *key, const int type, const char *value) {
    (void) game; (void) p;
    g_saves++;
    g_saved_type = type;
    snprintf(g_saved_key, sizeof(g_saved_key), "%s", key);
    snprintf(g_saved_value, sizeof(g_saved_value), "%s", value);
}
static void hook_load(Game *game, const Player *p) { (void) game; (void) p; g_loads++; }
static void hook_reward(Game *game, const Player *p, const int amount) { (void) game; (void) p; g_rewards++; g_reward_amount = amount; }

static void test_backend_actions(void) {
    Game g;
    // 0 Save Data "My,Score_Long_Name" type 9 (-> 5), 1 Save Data "Coins" from the player value "coins",
    // 2 Save Data "time", 3 Load Data, 4 Reward KR 25, 5 Save Data without a name
    make_game(&g, "{\"name\":\"t\"," SPAWNS ",\"objects\":["
        "{\"i\":29,\"p\":[0,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":49,\"tct\":\"My,Score_Long_Name\",\"tc2\":9}]},"
        "{\"i\":29,\"p\":[50,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":49,\"tct\":\"Coins\",\"tce\":2,\"tcc\":\"coins\",\"tc2\":3}]},"
        "{\"i\":29,\"p\":[100,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":49,\"tct\":\"TIME\",\"tc2\":2}]},"
        "{\"i\":29,\"p\":[150,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":57}]},"
        "{\"i\":29,\"p\":[200,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":32,\"tc\":25}]},"
        "{\"i\":29,\"p\":[250,0,0],\"s\":[10,10,10],\"te\":0,\"tas\":[{\"ta\":49,\"tc\":3}]}"
        "]}");
    Player *a = add_player(&g, 1);
    g_has_account = 1;
    // no backend: nothing happens (and nothing crashes)
    for (int i = 0; i < 6; i++) fire(&g, a, i);
    g.hooks.storage_save = hook_save;
    g.hooks.storage_load = hook_load;
    g.hooks.reward_kr = hook_reward;
    fire(&g, a, 0);
    CHECK(g_saves == 1 && !strcmp(g_saved_key, "myscore_long") && !strcmp(g_saved_value, "1") && g_saved_type == 5,
          "Save Data: lowercased, 12 chars, , removed, trigConst || 1, type clamped (%s %s %d)", g_saved_key, g_saved_value, g_saved_type);
    fire(&g, a, 1);
    CHECK(g_saves == 1, "Save Data: an undefined value saves nothing");
    player_custom_value_set(&g, a, "coins", 12);
    fire(&g, a, 1);
    CHECK(g_saves == 2 && !strcmp(g_saved_key, "coins") && !strcmp(g_saved_value, "12") && g_saved_type == 3, "Save Data: a player value");
    a->game_time = 0.0f;
    g.round_timer = 12.5f;
    fire(&g, a, 2);
    CHECK(g_saves == 3 && !strcmp(g_saved_key, "time") && !strcmp(g_saved_value, "12500"), "Save Data time: the round clock (%s)", g_saved_value);
    a->game_time = 4321.5f;
    fire(&g, a, 2);
    CHECK(g_saves == 4 && !strcmp(g_saved_value, "4321.5"), "Save Data time: the player's gameTime (%s)", g_saved_value);
    fire(&g, a, 5);
    CHECK(g_saves == 4, "Save Data without a name: nothing");
    fire(&g, a, 3);
    CHECK(g_loads == 1, "Load Data");
    fire(&g, a, 4);
    CHECK(g_rewards == 1 && g_reward_amount == 25, "Reward KR");
    g_has_account = 0;
    fire(&g, a, 0);
    fire(&g, a, 3);
    CHECK(g_saves == 4 && g_loads == 1, "no account: no Save / Load Data");
    g_has_account = 1;

    // the Load Data rows: numbers into custVals (+ "cv"), strings skipped, a full set stops the copy
    clear_sent();
    const char *keys[] = {"V_coins", "word", "trig"};
    const char *values[] = {"9", "abc", "2.5"};
    trigger_load_data_apply(&g, a, keys, values, 3);
    CHECK(cv(a, "V_coins") == 9.0 && cv(a, "trig") == 2.5 && !trigger_values_find(&a->custom_values, "word"), "Load Data rows");
    CHECK(count_sent(NET_PACKET_CUSTOM_VALUE) == 2, "cv sent per value: %d", count_sent(NET_PACKET_CUSTOM_VALUE));
    for (int i = 0; a->custom_values.count <= TRIGGER_V7_CUSTOM_VALUE_LIMIT; i++) {
        char k[16];
        snprintf(k, sizeof(k), "f%d", i);
        player_custom_value_set(&g, a, k, 1);
    }
    const char *more_keys[] = {"new1", "V_coins"};
    const char *more_values[] = {"1", "10"};
    trigger_load_data_apply(&g, a, more_keys, more_values, 2);
    CHECK(!trigger_values_find(&a->custom_values, "new1") && cv(a, "V_coins") == 9.0, "a full set ends the copy (custValLimit)");
    printf("backend actions done\n");
}

int main(void) {
    test_basic_actions();
    test_values_and_conditions();
    test_flow();
    test_events();
    test_pickups();
    test_reward_junk();
    test_legacy();
    test_backend_actions();
    test_editor_goldens();

    static const struct { const char *path; int triggers; } maps[] = {
        {"assets/maps/custom/ESP_Parkour.json", 95}, {"assets/maps/custom/Bhop_VIP.json", 54},
        {"assets/maps/custom/zombie_high.json", 284}, {"assets/maps/custom/Krunk_Plaza.json", 41},
        {"assets/maps/custom/Verb_Arena.json", 41}, {"assets/maps/custom/Slide_Neon_Stage.json", 54},
        {"assets/maps/custom/eterno_jump.json", 7}, {"assets/maps/custom/slide_moonlight.json", 5},
        {"assets/maps/311_soul_sanctum.json", 19},
    };
    for (size_t i = 0; i < sizeof(maps) / sizeof(maps[0]); i++) test_map(maps[i].path, maps[i].triggers);
    for (int i = 0; i < g_json_count; i++) cJSON_Delete(g_jsons[i]);

    if (g_failures) {
        printf("%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
