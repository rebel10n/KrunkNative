// Zombies (mode 35, Krunker v7.1.4 modes/zom.js; doc/v7/map-logic.md §4.1b). WP-D shipped the mode
// shell (table entry, condition / winCondition); map-logic WP-H stage 2 fills the waves, the AI
// templates (map_ai_spawn at zombie_spawn SERVER_NODEs), the power-up drops, the perk bottle, the
// Zombie Portal / skull easter eggs and the spawnable pickups. The machines, power-up timers and
// finiteAmmo reserves are in shared/src/zombies.c.
//
// v7.1.4 bugs fixed here (doc/v7/decisions.md "v7 bugs"):
// - the wave growth caps Runners with `zombCnts[3] = Math.min(zombCnts[2], 4)` (index 2): the
//   Runner count copied the Flesh Colossal count. KN caps zombCnts[3] at 4 as intended.
// - a downed player's revive score goes through players.score(.., set = true), which doubles it
//   during Double Points; KN sets half the score.
#include "mode_internal.h"
#include <zombies.h>
#include <map_ai.h>
#include <triggers_v7.h>
#include <ai.h>
#include <cJSON.h>
#include <math.h>
#include <string.h>

// zom.js vA118 (Walker, Brute, Flesh Colossal, Runner, Rascal) without "p" (the node decides)
static const char *const k_templates[] = {
    "{\"s\":[1.3,7.21,7.45],\"i\":55,\"h\":200,\"cdy\":1,\"tm\":2,\"tAi\":1,\"ct\":1,\"te\":15,\"mro\":270,\"tas\":[{\"ta\":60,\"tct\":\"4280|0.1,4041|0.1,5728|0.1,3238|0.25\"}],\"rsc\":50,\"sd\":1,\"ms\":1.4,\"aid\":\"46802q\",\"rmt\":5000,\"rmr\":60,\"aic\":10,\"tsd\":1,\"spd\":0.8,\"prs\":1,\"prg\":0,\"prsc\":1,\"prms\":1,\"pjs\":1,\"pjd\":100,\"vd\":10000,\"idl\":32601,\"dth\":0,\"atk\":0,\"atkh\":0,\"mel\":32600,\"melh\":0,\"agr\":0,\"fr\":2000,\"aac\":0,\"shb\":400,\"sha\":0,\"shdy\":0,\"mldy\":300,\"mls\":0,\"ss\":0,\"crd\":1.5,\"crh\":2.8,\"hr\":1000,\"mlr\":15,\"mlra\":1.5,\"mld\":40,\"grv\":1,\"bh\":0,\"pht\":0,\"phi\":\"\",\"cnh\":1,\"xv\":1,\"ain\":\"Walker\",\"ains\":1,\"hbh\":2.7,\"hbw\":0.9,\"ail\":10,\"sp\":25,\"as\":0.8,\"sor\":0.8,\"pmv\":1,\"dos\":1}",
    "{\"s\":[1.6900000000000002,9.373000000000001,9.685],\"i\":55,\"h\":550,\"cdy\":1,\"tm\":2,\"tAi\":1,\"ct\":1,\"te\":15,\"mro\":270,\"tas\":[{\"ta\":60,\"tct\":\"4280|0.1,4041|0.1,5728|0.1,3238|0.25\"}],\"rsc\":50,\"sd\":1,\"ms\":1.8199999999999998,\"aid\":\"46802q\",\"rmt\":5000,\"rmr\":60,\"aic\":10,\"tsd\":1,\"spd\":0.35,\"prs\":1,\"prg\":0,\"prsc\":1,\"prms\":1,\"pjs\":1,\"pjd\":100,\"vd\":10000,\"idl\":32601,\"dth\":0,\"atk\":0,\"atkh\":0,\"mel\":32600,\"melh\":0,\"agr\":0,\"fr\":2000,\"aac\":0,\"shb\":400,\"sha\":0,\"shdy\":0,\"mldy\":500,\"mls\":0,\"ss\":0,\"crd\":1.5,\"crh\":3.1,\"hr\":1100,\"mlr\":20,\"mlra\":1.5,\"mld\":60,\"grv\":1,\"bh\":0,\"pht\":0,\"phi\":\"\",\"cnh\":1,\"xv\":1,\"ain\":\"Brute\",\"ains\":1,\"hbh\":2.7,\"hbw\":0.9,\"ail\":30,\"sp\":35,\"as\":0.6,\"sor\":0.6,\"pmv\":1,\"dos\":1}",
    "{\"s\":[1.95,10.815,11.175],\"i\":55,\"h\":2500,\"cdy\":1,\"tm\":2,\"tAi\":1,\"ct\":1,\"te\":15,\"mro\":270,\"tas\":[{\"ta\":60,\"tct\":\"4280|0.5,4041|0.5,5728|0.5,3238|1,5819|0.1\"}],\"rsc\":50,\"sd\":1,\"ms\":1.8,\"aid\":\"46803q\",\"rmt\":5000,\"rmr\":60,\"aic\":10,\"tsd\":0.4,\"spd\":0.35,\"prs\":1,\"prg\":0,\"prsc\":1,\"prms\":1,\"pjs\":1,\"pjd\":100,\"vd\":10000,\"idl\":32601,\"dth\":0,\"atk\":0,\"atkh\":0,\"mel\":32600,\"melh\":0,\"agr\":0,\"fr\":2000,\"aac\":0,\"shb\":400,\"sha\":0,\"shdy\":0,\"mldy\":700,\"mls\":0.1,\"ss\":0,\"crd\":1.5,\"crh\":4.9,\"hr\":1500,\"mlr\":30,\"mlra\":3.141592653589793,\"mld\":100,\"grv\":1,\"bh\":0,\"pht\":0,\"phi\":\"\",\"cnh\":1,\"xv\":1,\"ain\":\"Flesh Colossal\",\"ains\":1.5,\"hbh\":3.5,\"hbw\":1.8,\"ail\":70,\"sp\":150,\"as\":1,\"sor\":0.35,\"pmv\":1,\"dos\":1}",
    "{\"s\":[1.3,7.21,7.45],\"i\":55,\"h\":200,\"cdy\":1,\"tm\":2,\"tAi\":1,\"ct\":1,\"te\":15,\"mro\":270,\"tas\":[{\"ta\":60,\"tct\":\"4280|0.25,4041|0.25,5728|0.25,3238|0.5\"}],\"rsc\":50,\"sd\":1,\"ms\":1.4,\"aid\":\"46804q\",\"rmt\":5000,\"rmr\":60,\"aic\":10,\"tsd\":1.8,\"spd\":1.4,\"prs\":1,\"prg\":0,\"prsc\":1,\"prms\":1,\"pjs\":1,\"pjd\":100,\"vd\":10000,\"idl\":32601,\"dth\":0,\"atk\":0,\"atkh\":0,\"mel\":32600,\"melh\":0,\"agr\":0,\"fr\":2000,\"aac\":0,\"shb\":400,\"sha\":0,\"shdy\":0,\"mldy\":300,\"mls\":0,\"ss\":0,\"crd\":1.5,\"crh\":2.1,\"hr\":1000,\"mlr\":15,\"mlra\":1.5,\"mld\":30,\"grv\":1,\"bh\":0,\"pht\":0,\"phi\":\"\",\"cnh\":1,\"xv\":1,\"ain\":\"Runner\",\"ains\":1,\"hbh\":2.5,\"hbw\":1,\"ail\":10,\"sp\":25,\"as\":0.8,\"sor\":1,\"pmv\":1,\"dos\":1}",
    "{\"s\":[1.3,7.21,7.45],\"i\":55,\"h\":200,\"cdy\":1,\"tm\":2,\"tAi\":1,\"ct\":1,\"te\":15,\"mro\":270,\"tas\":[{\"ta\":60,\"tct\":\"4280|0.1,4041|0.1,5728|0.1,3238|0.25\"}],\"rsc\":50,\"sd\":1,\"ms\":0.5,\"aid\":\"46803q\",\"rmt\":5000,\"rmr\":60,\"aic\":10,\"tsd\":3.6,\"spd\":1.4,\"prs\":1,\"prg\":0,\"prsc\":1,\"prms\":1,\"pjs\":1,\"pjd\":100,\"vd\":10000,\"idl\":32601,\"dth\":0,\"atk\":0,\"atkh\":0,\"mel\":32600,\"melh\":0,\"agr\":0,\"fr\":2000,\"aac\":0,\"shb\":400,\"sha\":0,\"shdy\":0,\"mldy\":300,\"mls\":0,\"ss\":0,\"crd\":1.5,\"crh\":2.1,\"hr\":1000,\"mlr\":15,\"mlra\":1.5,\"mld\":30,\"grv\":1,\"bh\":0,\"pht\":0,\"phi\":\"\",\"cnh\":1,\"xv\":1,\"ain\":\"Rascal\",\"ains\":1,\"hbh\":2.5,\"hbw\":1,\"ail\":1,\"sp\":25,\"as\":0.8,\"sor\":1,\"pmv\":1,\"dos\":1}",
};
#define ZOMBIE_TEMPLATES ((int) (sizeof(k_templates) / sizeof(k_templates[0])))
static const char *const k_template_names[] = {"Walker", "Brute", "Flesh Colossal", "Runner", "Rascal"};

// zom.js vA119: special waves (every 5th)
static const struct {
    const char *name;
    int counts[4];
} k_specials[] = {
    {"FRENZY", {0, 2, 0, 10}},
    {"ONSLAUGHT", {0, 15, 1, 1}},
    {"DEVASTATION", {2, 2, 5, 5}},
    {"ANNIHILATION", {5, 5, 10, 5}},
    {"LIMBO", {0, 0, 15, 10}},
};
#define ZOMBIE_SPECIALS ((int) (sizeof(k_specials) / sizeof(k_specials[0])))

static const int k_powerup_rarities[4] = {89, 5, 4, 2}; // powerupRarities
static const int k_powerups[4][3] = {                    // powerups
    {0},
    {SPAWNABLE_AMMO_CACHE, SPAWNABLE_DOUBLE_POINTS},
    {SPAWNABLE_MINIGUN, SPAWNABLE_GRENADE_LAUNCHER},
    {SPAWNABLE_NUKE, SPAWNABLE_INSTA_KILL, SPAWNABLE_DISCOUNT},
};
static const int k_powerup_counts[4] = {0, 2, 2, 3};

static int node_tagged(const Object *o, const char *tag) {
    return o && o->server_node && o->node_tag && !strcmp(o->node_tag, tag);
}

// map.manager.serverNodes.filter(tag)
static int nodes(const Game *game, const char *tag, const Object **out, const int max) {
    int n = 0;
    for (size_t i = 0; game->map && i < game->map->server_node_count && n < max; i++) {
        const size_t index = game->map->server_nodes[i];
        const Object *o = index < game->map->object_count ? game->map->objects[index] : NULL;
        if (node_tagged(o, tag)) out[n++] = o;
    }
    return n;
}

static int clock_running(const Game *game) {
    return !game->mode->wait_timer_count || game->mode->wait_timers[0].cont_time;
}

static float rand_percent(void) {
    return roundf(mode_random() * 10000.0f) / 100.0f; // UTILS.randFloat(0, 100).round(2)
}

// zom.js gameStart
void mode_zombies_game_start(Game *game) {
    GameZombies *z = &game->zombies;
    z->spawn_timer = 0.0f;
    z->skip_check = 0;
    z->wave = 0;
    z->conc = 5;
    const int counts[4] = {10, 0, 0, 1};
    memcpy(z->counts, counts, sizeof(counts));
    z->pending_count = 0;
    z->left = 0;
    z->hp_mlt = z->dmg_mlt = z->spd_mlt = 1.0f;
    z->frog_round = mode_rand_int(4, 7);
    z->drone_round = mode_rand_int(9, 13);
    z->pap_completed = z->pap_active = 0;
    const int order[3] = {0, 1, 3}; // ["Walker", "Brute", "Runner"], yateShuffle
    memcpy(z->pap_order, order, sizeof(order));
    for (int i = 2; i > 0; i--) {
        const int j = mode_rand_int(0, i);
        const int t = z->pap_order[i];
        z->pap_order[i] = z->pap_order[j];
        z->pap_order[j] = t;
    }
    z->pap_progress = 0;
    z->dance_timer = 0.0f;
    z->dance_completed = 0;
    z->dance_skulls = 0;
}

// One AI of template `kind` at a node (map.manager.addAI(.., true) + AI.spawn): hp x hpMlt,
// melee damage x dmgMlt, speed x spdMlt; the level shows the portal order until it is done.
static int spawn_zombie(Game *game, const int kind, const Object *node) {
    GameZombies *z = &game->zombies;
    cJSON *data = cJSON_Parse(k_templates[kind]);
    if (!data) return -1;
    const double h = cJSON_GetNumberValue(cJSON_GetObjectItem(data, "h"));
    const double mld = cJSON_GetNumberValue(cJSON_GetObjectItem(data, "mld"));
    const double spd = cJSON_GetNumberValue(cJSON_GetObjectItem(data, "spd"));
    cJSON_SetNumberValue(cJSON_GetObjectItem(data, "h"), round(h * z->hp_mlt));
    cJSON_SetNumberValue(cJSON_GetObjectItem(data, "mld"), round(mld * z->dmg_mlt));
    cJSON_SetNumberValue(cJSON_GetObjectItem(data, "spd"), spd * z->spd_mlt);
    if (!z->pap_completed) { // ail + eePAPOrder.indexOf(ain) + 1
        int index = -1;
        for (int i = 0; i < 3; i++) {
            if (z->pap_order[i] == kind) index = i;
        }
        const double level = cJSON_GetNumberValue(cJSON_GetObjectItem(data, "ail"));
        cJSON_SetNumberValue(cJSON_GetObjectItem(data, "ail"), level + index + 1);
    }
    const int sid = map_ai_spawn(game, data, node->position, -1);
    cJSON_Delete(data);
    return sid;
}

static void next_wave(Game *game) {
    GameZombies *z = &game->zombies;
    z->wave++;
    z->skip_check = 1;
    z->spawn_timer = 7500.0f;
    for (size_t i = 0; i < game->player_count; i++) { // the downed come back with half their score
        Player *p = game->players[i];
        if (!p || p->active) continue;
        p->mode.lives = 1;
        mode_send_lives(game, p);
        p->score = (int) roundf((float) p->score * 0.5f);
        const NetScorePacket score = {.delta = 0, .score = p->score};
        game_send(game, p->uid, NET_PACKET_SCORE, &score, sizeof(score));
        game->leaders_dirty = 1;
        if (game->hooks.respawn) game->hooks.respawn(game, p->uid); // "frs": enterGame
    }
    // (every zDroneSpawnRound-th wave a 0.01% account skin: giveSkin has no inventory in KN)
    if (z->wave % z->frog_round == 0 && rand_percent() >= 80.0f) {
        const Object *frogs[64];
        const int n = nodes(game, "frog_spawn", frogs, 64);
        if (n) spawnable_drop(game, SPAWNABLE_FROG, NULL, 0, frogs[mode_rand_int(0, n - 1)]->position);
    }
    z->conc = z->conc + 2 < 30 ? z->conc + 2 : 30;
    static const int caps[4] = {30, 10, 5, 4};
    const int grow[4] = {z->wave % 2 == 0, z->wave % 2 == 0, z->wave % 4 == 0, z->wave % 3 == 0};
    for (int i = 0; i < 4; i++) {
        z->counts[i] += grow[i];
        if (z->counts[i] > caps[i]) z->counts[i] = caps[i];
    }
    const int special = z->wave % 5 == 0 ? mode_rand_int(0, ZOMBIE_SPECIALS - 1) : -1;
    const int *counts = special >= 0 ? k_specials[special].counts : z->counts;
    z->pending_count = 0;
    z->left = 0;
    for (int i = 0; i < 4; i++) {
        z->left += counts[i];
        for (int k = 0; k < counts[i] && z->pending_count < ZOMBIES_MAX_PENDING; k++) z->pending[z->pending_count++] = (unsigned char) i;
    }
    z->hp_mlt += z->wave % 2 == 0 ? 0.1f : 0.0f;
    z->dmg_mlt += z->wave % 4 == 0 ? 0.1f : 0.0f;
    z->spd_mlt += z->wave % 4 == 0 ? 0.1f : 0.0f;

    NetZombieWavePacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.wave = z->wave;
    if (special >= 0) snprintf(packet.special, sizeof(packet.special), "%s", k_specials[special].name);
    memcpy(z->special, packet.special, sizeof(z->special));
    game_send(game, NET_TARGET_ALL, NET_PACKET_ZOMBIE_WAVE, &packet, sizeof(packet)); // "upz"
}

// zom.js update: every 10 s, a new wave when nothing is alive or left to spawn, else spawn up to
// concZomb at random zombie_spawn nodes
void mode_zombies_update(Game *game, const float delta) {
    GameZombies *z = &game->zombies;
    if (!clock_running(game)) return;
    const Object *spawns[64];
    const int spawn_count = nodes(game, "zombie_spawn", spawns, 64);
    if (!spawn_count) return;
    const float delta_ms = delta * 1000.0f;
    if (z->dance_timer > 0.0f) {
        z->dance_timer -= delta_ms;
        if (z->dance_timer <= 0.0f) {
            z->dance_timer = 0.0f;
            if (!z->dance_completed) mode_send_text_popup(game, "Souls escaped", "");
        }
    }
    z->spawn_timer -= delta_ms;
    if (z->spawn_timer > 0.0f) return;
    z->spawn_timer = 10000.0f;
    const int alive = game_ai_active_count(game, 2);
    if (alive <= 0 && z->left <= 0 && !z->skip_check) {
        next_wave(game);
        return;
    }
    z->skip_check = 0;
    const int room = z->conc - alive;
    for (int i = 0; i < room && z->pending_count; i++) {
        const int pick = mode_rand_int(0, z->pending_count - 1); // tmpCnts.splice(randInt, 1)
        const int kind = z->pending[pick];
        memmove(&z->pending[pick], &z->pending[pick + 1], (size_t) (z->pending_count - pick - 1));
        z->pending_count--;
        spawn_zombie(game, kind, spawns[mode_rand_int(0, spawn_count - 1)]);
        z->left--;
    }
}

// zom.js onKill: a player died, so everyone in the game (and the victim) with a positive score
// loses 10% of it.
void mode_zombies_on_kill(Game *game, Player *victim, Player *killer, const int weapon_id) {
    (void) killer;
    (void) weapon_id;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (!player || !(player->active || player == victim) || player->score <= 0) continue;
        const int loss = player->score / 10; // Math.floor(score * 0.1)
        if (loss > 0) player_score(player, -loss, 0);
    }
}

// zom.js onPreEnd: everyone's score becomes (zWave || 1) * 100
void mode_zombies_on_pre_end(Game *game) {
    const int wave = game->zombies.wave ? game->zombies.wave : 1;
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        if (player) player->score = wave * 100;
    }
}

// zom.js onAIDeath: the Ammo Cache on the last kill of every 5th wave; for a player's kill a
// power-up (rarities [89, 5, 4, 2]) and the 1% Perk Bottle
void mode_zombies_on_ai_death(Game *game, struct GameAI_t *ai, Player *killer) {
    const GameZombies *z = &game->zombies;
    if (z->wave && z->wave % 5 == 0 && z->left <= 0 && game_ai_active_count(game, 2) <= 0) {
        spawnable_drop(game, SPAWNABLE_AMMO_CACHE, NULL, 0, ai->position);
    }
    if (!killer) return;
    float roll = rand_percent();
    if (k_powerup_rarities[0] == 0) roll = fmaxf(roll, 0.01f);
    int tier = 0, sum = 0;
    for (int i = 0; i < 4; i++) {
        sum += k_powerup_rarities[i];
        if (roll <= (float) sum) {
            tier = i;
            break;
        }
    }
    if (k_powerup_counts[tier]) spawnable_drop(game, k_powerups[tier][mode_rand_int(0, k_powerup_counts[tier] - 1)], NULL, 0, ai->position);
    if (rand_percent() >= 99.0f) spawnable_drop(game, SPAWNABLE_PERK_BOTTLE, NULL, 0, ai->position);
}

// zom.js onTrigger (action 71): the Zombie Portal at the portal_spawn node, once
void mode_zombies_on_trigger(Game *game, Player *player, const int value) {
    (void) player;
    (void) value;
    GameZombies *z = &game->zombies;
    if (z->pap_active) return;
    z->pap_active = 1;
    const Object *portal[1];
    if (nodes(game, "portal_spawn", portal, 1)) spawnable_drop(game, SPAWNABLE_ZOMBIE_PORTAL, NULL, 0, portal[0]->position);
}

void mode_zombies_portal_closed(Game *game) {
    GameZombies *z = &game->zombies;
    if (z->pap_completed) return;
    z->pap_active = 0;
    trigger_global_value_set(game, "portal", 0.0); // globalVals.portal = 0 + "gv" (WP-C)
}

static int template_of(const char *name) {
    for (int i = 0; i < ZOMBIE_TEMPLATES; i++) {
        if (!strcmp(name, k_template_names[i])) return i;
    }
    return -1;
}

// Zombie Portal onPickup(ai): zombies walking in in eePAPOrder complete the easter egg (the
// interface 525 object breaks, a 122 s dance timer, skulls at the skull_spawn nodes); a wrong
// one restarts the order.
static void portal_pickup(Game *game, GameAI *ai) {
    GameZombies *z = &game->zombies;
    if (z->pap_progress >= 3 || template_of(ai->name) != z->pap_order[z->pap_progress]) {
        z->pap_progress = 0;
        game_play_sound(game, "mystery_1", -1, ai->position, 2.0f, 0.0f);
        return;
    }
    z->pap_progress++;
    game_play_sound(game, "collect_1", -1, ai->position, 2.0f, 0.0f);
    game_ai_kill(game, ai, NULL);
    if (z->pap_progress < 3) return;
    z->pap_completed = 1;
    game_play_sound(game, "cheer_1", -1, ai->position, 2.0f, 0.0f);
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        Object *o = game->map->objects[i];
        if (o->can_destroy && !o->destroyed && o->interface_id == 525) {
            game_destroy_object(game, o, NULL);
            break;
        }
    }
    z->dance_timer = 122000.0f;
    const Object *skulls[64];
    const int count = nodes(game, "skull_spawn", skulls, 64);
    for (int i = 0; i < count; i++) spawnable_drop(game, SPAWNABLE_SKULL_EE, NULL, 0, skulls[i]->position);
}

// Skull EE onShoot: the last skull makes the AIs dance 10 s, drops 4 Free Upgrades / Perk
// Bottles around the first spawn and brings the interface 252 objects back
static void skull_shot(Game *game) {
    GameZombies *z = &game->zombies;
    z->dance_skulls++;
    const Object *skulls[64];
    if (z->dance_skulls < nodes(game, "skull_spawn", skulls, 64) || z->dance_completed) return;
    z->dance_completed = 1;
    z->dance_timer = 0.0f;
    GameAIs *list = game_ais(game);
    if (list) list->force_dance_timer = 10000.0f;
    if (game->map && game->map->spawn_count) {
        const vec3 at = game->map->spawns[0]->position;
        static const int drops[2] = {SPAWNABLE_FREE_UPGRADE, SPAWNABLE_PERK_BOTTLE};
        for (int i = 0; i < 4; i++) {
            const int type = drops[mode_rand_int(0, 1)];
            const float dx = (float) mode_rand_int(15, 25) * (mode_rand_int(0, 1) ? 1.0f : -1.0f);
            const float dz = (float) mode_rand_int(15, 25) * (mode_rand_int(0, 1) ? 1.0f : -1.0f);
            spawnable_drop(game, type, NULL, 0, (vec3) {at.x + dx, at.y, at.z + dz});
        }
    }
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        Object *o = game->map->objects[i];
        if (o->destroyed && o->interface_id == 252) game_respawn_object(game, o, NULL);
    }
}

// medals.js onPickup / onShoot of the Zombies spawnables (spawnables.c calls it)
void mode_zombies_spawnable(Game *game, Player *player, struct GameAI_t *ai, const int type) {
    switch (type) {
        case SPAWNABLE_INSTA_KILL: zombies_set_powerup(game, ZOMBIES_POWERUP_INSTA_KILL, 30000.0f); break;
        case SPAWNABLE_DISCOUNT: zombies_set_powerup(game, ZOMBIES_POWERUP_FIRE_SALE, 20000.0f); break;
        case SPAWNABLE_DOUBLE_POINTS: zombies_set_powerup(game, ZOMBIES_POWERUP_DOUBLE_POINTS, 30000.0f); break;
        case SPAWNABLE_MINIGUN: // giveWeapon(player, 25, false, true)
            if (player) zombies_give_weapon(player, WEAPON_ID_MINIGUN, 0);
            break;
        case SPAWNABLE_GRENADE_LAUNCHER: // giveWeapon(player, 26, false, true): the War Machine launcher
            if (player) zombies_give_weapon(player, WEAPON_ID_WAR_MACHINE, 0);
            break;
        case SPAWNABLE_PERK_BOTTLE: { // a random perk the player lacks
            if (!player) break;
            int options[ZOMBIES_PERK_COUNT], count = 0;
            for (int i = 0; i < ZOMBIES_PERK_COUNT; i++) {
                if (!player_has_perk(player, i)) options[count++] = i;
            }
            if (!count) break;
            player_give_perk(player, options[mode_rand_int(0, count - 1)]);
            game_play_sound(game, "asset:30220", -1, player->position, 2.0f, 0.0f);
            break;
        }
        case SPAWNABLE_FREE_UPGRADE: { // the held weapon comes back upgraded
            if (!player || !player->loadout || player->loadout_index < 0 || player->loadout_index >= player->loadout_size) break;
            const int weapon = player->loadout[player->loadout_index];
            game_play_sound(game, "collect_1", player->uid, player->position, 1.2f, 0.0f);
            player_remove_weapon(player, weapon);
            zombies_give_weapon(player, weapon, 1);
            break;
        }
        case SPAWNABLE_ZOMBIE_PORTAL:
            if (ai) portal_pickup(game, ai);
            break;
        case SPAWNABLE_SKULL_EE:
            if (player) game_play_sound(game, "collect_1", player->uid, player->position, 1.2f, 0.0f);
            skull_shot(game);
            break;
        default:
            break;
    }
}
