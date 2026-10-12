// Krunker v7.1.4 airdrops (map-logic WP-H stage 2, doc/v7/map-logic.md §4.4): streak 1
// "Airdrop" calls a package in from 500 units up (streaks.js), it falls at 0.05 u/ms
// (data/airDrop.js) and a player holding interact on the ground within 6 units claims it
// (player.js :1718-1728, game.claimAirDrop). Packages and tiers: data/airDropPackages.js.
//
// KN policy (doc/v7/decisions.md, map-logic §4.4 "KN"): public lobbies only. Never offline, and
// never in a hosted game or the sandbox (game_config_is_custom), where v7's activate returns
// false too. Maps cannot place or configure them.
//
// KN notes:
// - The KR / Junk / Skin packages pay into the account (additionalReward, sendUserJunk,
//   giveSkin). KN has no backend call for that yet (decisions.md "Backend-only triggers"), so
//   they are claimed (the "krp" line goes out) but pay nothing.
// - The trapped explosion goes through game_explosion, which hurts its source by a fifth; v7
//   skips the source of a streak explosion (data.stk). The source is the drop's owner, who sees
//   the real package and so normally doesn't claim it.
// - v7 resends "ad" to every viewer it has not reached every airDSyncT ticks (sentTo). KN sends
//   it once to everyone when the drop starts and to joiners in their join sync.
#include <zombies.h>
#include <account.h>
#include <ai.h>
#include <world.h>
#include <game_config.h>
#include "modes/mode_internal.h"
#include <math.h>
#include <string.h>

const AirDropPackage g_airdrop_packages[AIRDROP_PACKAGE_COUNT] = {
    {"KR Package", 0, 0, 2500.0f, -1},
    {"KR Package", 1, 0, 2500.0f, -1},
    {"KR Package", 2, 0, 2500.0f, -1},
    {"KR Package", 3, 0, 2500.0f, -1},
    {"KR Package", 4, 0, 3000.0f, -1},
    {"KR Package", 5, 0, 3000.0f, -1},
    {"KR Package", 6, 0, 4500.0f, -1, .animate = 1},
    {"Junk Package", 0, 3, 2500.0f, -1},
    {"Junk Package", 1, 3, 2500.0f, -1},
    {"Junk Package", 2, 3, 2500.0f, -1},
    {"Junk Package", 3, 3, 2500.0f, -1},
    {"Junk Package", 4, 3, 3000.0f, -1},
    {"Junk Package", 5, 3, 3000.0f, -1},
    {"Junk Package", 6, 3, 4500.0f, -1, .animate = 1},
    {"Trapped Package", 1, 1, 1000.0f, -1, .imitate = 1}, // Poisoned
    {"Trapped Package", 2, 1, 1000.0f, -1, .imitate = 1}, // Rooted + Dizzy
    {"Trapped Package", 3, 1, 1000.0f, -1, .imitate = 1}, // explosion
    {"Skin Package", 4, 2, 3000.0f, -1},
    {"Juggernaut Package", 5, 5, 3000.0f, STREAK_JUGGERNAUT},
    {"War Machine Package", 4, 6, 3000.0f, STREAK_WAR_MACHINE},
    {"Nuke Package", 6, 7, 3000.0f, STREAK_NUKE, .animate = 1},
    {"Slimer Package", 3, 8, 3000.0f, STREAK_SLIMER},
};

// vA764: Uncommon, Rare, Epic, Legendary, Relic, Contraband, Unobtainable
const char *const g_airdrop_rarity_colors[7] = {"#b2f252", "#2196F3", "#E040FB", "#FBC02D", "#ed4242", "#292929", "#fff53d"};

// vA763: Starter (< 25), Elite (>= 25), Heroic (>= 50)
static const float k_tiers[3][7] = {
    {75.0f, 22.0f, 3.0f, 0.0f, 0.0f, 0.0f, 0.0f},
    {50.0f, 30.0f, 15.0f, 5.0f, 0.0f, 0.0f, 0.0f},
    {0.0f, 48.0f, 35.0f, 14.0f, 2.49f, 0.5f, 0.01f},
};
static const int k_tier_sizes[3] = {6, 6, 7};

int airdrop_allowed(const Game *game) {
    if (!game || !game_is_authority(game) || game->is_local) return 0;
#ifdef GAME_CONFIG_V7
    if (game_config_is_custom(game)) return 0; // isCustom: hosted games and the sandbox
#endif
    return 1;
}

// mode.disabledStreaks literal semantics (airDrop.js:80): a streak package only when the mode
// has the list and the streak is not in it, so never in the modes without one.
static int package_streak_ok(const Game *game, const AirDropPackage *p) {
    if (p->streak < 0) return 1;
    const GameModeConfig *c = game && game->mode ? &game->mode->config : NULL;
    return c && c->disabled_streaks && !mode_streak_disabled(game, p->streak);
}

int airdrop_pick_package(const Game *game, const int level, int *imitate) {
    const int tier = level >= 50 ? 2 : level >= 25 ? 1 : 0;
    float roll = roundf(mode_random() * 10000.0f) / 100.0f; // randFloat(0, 100).round(2)
    if (k_tiers[tier][0] == 0.0f) roll = fmaxf(roll, 0.01f);
    int rarity = 0;
    float sum = 0.0f;
    for (int i = 0; i < k_tier_sizes[tier]; i++) {
        sum += k_tiers[tier][i];
        if (roll <= sum) {
            rarity = i;
            break;
        }
    }
    int options[AIRDROP_PACKAGE_COUNT], count = 0;
    for (int i = 0; i < AIRDROP_PACKAGE_COUNT; i++) {
        if (g_airdrop_packages[i].rarity == rarity && package_streak_ok(game, &g_airdrop_packages[i])) options[count++] = i;
    }
    const int index = count ? options[mode_rand_int(0, count - 1)] : 0; // getRandom([]) -> dropIndex 0
    if (imitate) {
        *imitate = -1;
        if (g_airdrop_packages[index].imitate) {
            int real[AIRDROP_PACKAGE_COUNT], n = 0;
            for (int i = 0; i < AIRDROP_PACKAGE_COUNT; i++) {
                if (!g_airdrop_packages[i].imitate) real[n++] = i;
            }
            *imitate = real[mode_rand_int(0, n - 1)];
        }
    }
    return index;
}

// calcFallDist: canSee(drop, x, y - 1000, z, 0, 0, true, true, true) || 1, times 1000. The drop
// has no height, so the ray starts cameraHeight below it.
static float calc_fall(const Game *game, const vec3 position) {
    const vec3 origin = {position.x, position.y - game_constants.camera_height, position.z};
    const vec3 segment = {1e-6f, -1000.0f, 1e-6f}; // straight down (no exact zero: line_in_rect divides)
    float nearest = 0.0f;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!o->active || o->complex_coll || (o->no_shoot && !o->ramp)) continue;
        const float t = line_in_rect(origin, segment, o->position, o->scale);
        if (t > 0.0f && t < 1.0f && (!nearest || t < nearest)) nearest = t;
    }
    const float terrain = world_terrain_fraction(game, origin, segment);
    if (terrain > 0.0f && terrain < 1.0f && (!nearest || terrain < nearest)) nearest = terrain;
    return (nearest ? nearest : 1.0f) * 1000.0f;
}

static void drop_send(Game *game, const AirDrop *d, const int uid, const int as_owner) {
    const NetAirDropPacket packet = {
        .sid = d->sid,
        .fall = d->fall,
        .position = {roundf(d->position.x * 100.0f) / 100.0f, d->position.y, roundf(d->position.z * 100.0f) / 100.0f},
        .index = (uint8_t) (d->imitate >= 0 && !as_owner ? d->imitate : d->index),
        .owner_uid = d->owner_uid,
    };
    game_send(game, uid, NET_PACKET_AIRDROP, &packet, sizeof(packet));
}

// AirDrop.sync for one viewer: the owner sees the real package, everyone else a trap's disguise
static void drop_send_viewers(Game *game, const AirDrop *d) {
    for (size_t i = 0; i < game->player_count; i++) {
        const Player *p = game->players[i];
        if (p) drop_send(game, d, p->uid, p->uid == d->owner_uid);
    }
    drop_send(game, d, NET_TARGET_NON_PLAYERS, 0);
}

int airdrop_drop(Game *game, Player *owner, const vec3 position, const float fall, const int package) {
    if (!game) return -1;
    GameZombies *z = &game->zombies;
    int slot = -1;
    for (int i = 0; i < z->drop_count; i++) {
        if (!z->drops[i].active) slot = i;
    }
    if (slot < 0) {
        if (z->drop_count >= AIRDROP_MAX) return -1;
        slot = z->drop_count++;
    }
    AirDrop *d = &z->drops[slot];
    memset(d, 0, sizeof(*d));
    d->sid = slot + 1;
    d->owner_uid = owner ? owner->uid : -1;
    d->position = position;
    d->active = 1;
    d->imitate = -1;
    d->fall = fall < 0.0f ? calc_fall(game, position) : fall;
    if (package >= 0 && package < AIRDROP_PACKAGE_COUNT) { // a forced package (tests)
        d->index = package;
        if (g_airdrop_packages[package].imitate) d->imitate = package == 14 ? 0 : 7; // a KR / Junk look
    } else {
        AccountInfo account;
        game_account_info(game, owner, &account);
        d->index = airdrop_pick_package(game, account.level, &d->imitate);
    }
    d->claim_time = g_airdrop_packages[d->index].claim_time;
    if (d->fall <= 0.0f) d->can_claim = 1;
    if (game_is_authority(game)) drop_send_viewers(game, d); // (the clients play adrop_0 on "ad")
    return d->sid;
}

// streaks.js Airdrop.activate: a clear sky above the caller, else a chat line
int airdrop_streak_activate(Game *game, Player *player) {
    if (!airdrop_allowed(game) || !player || !player->active) return 0;
    const vec3 sky = {player->position.x, player->position.y + AIRDROP_HEIGHT, player->position.z};
    if (!game_ai_player_can_see(game, player, sky)) {
        NetChatPacket chat;
        memset(&chat, 0, sizeof(chat));
        chat.uid = -1;
        chat.kind = NET_CHAT_SERVER;
        snprintf(chat.text, sizeof(chat.text), "This place is not suitable for an Airdrop");
        game_send(game, player->uid, NET_PACKET_CHAT, &chat, sizeof(chat));
        return 0;
    }
    return airdrop_drop(game, player, sky, -1.0f, -1) >= 0;
}

static void drop_remove_send(Game *game, const AirDrop *d) {
    const NetAirDropRemovePacket packet = {.sid = d->sid};
    game_send(game, NET_TARGET_ALL, NET_PACKET_AIRDROP_REMOVE, &packet, sizeof(packet)); // "rad"
}

static Player *find_player(Game *game, const int uid) {
    for (size_t i = 0; i < game->player_count; i++) {
        if (game->players[i] && game->players[i]->uid == uid) return game->players[i];
    }
    return NULL;
}

static int effect_max(const Game *game, const int effect) {
    return game->effects ? game->effects[effect].max : g_effects[effect].max;
}

// airDropPackages[i].claim
static void package_claim(Game *game, AirDrop *d, Player *claimer) {
    const AirDropPackage *p = &g_airdrop_packages[d->index];
    if (p->streak >= 0) { // streaks[s].activate (account needed; the offline sandbox never gets here)
        if (!claimer->has_account || !claimer->active) return;
        switch (p->streak) {
            case STREAK_NUKE: game_start_nuke(game, claimer); break;
            case STREAK_JUGGERNAUT: if (!player_has_equipment(claimer)) player_set_juggernaut(claimer, 1); break;
            case STREAK_WAR_MACHINE: if (!player_has_equipment(claimer)) player_give_weapon(claimer, WEAPON_ID_WAR_MACHINE); break;
            case STREAK_SLIMER: if (!player_has_equipment(claimer)) player_give_weapon(claimer, WEAPON_ID_SLIMER); break;
            default: break;
        }
        return;
    }
    switch (d->index) {
        case 14:
            game_play_sound(game, "taunt_2", claimer->uid, claimer->position, 1.0f, 0.0f);
            player_give_effect(claimer, EFFECT_POISONED, effect_max(game, EFFECT_POISONED));
            break;
        case 15:
            game_play_sound(game, "taunt_3", claimer->uid, claimer->position, 1.0f, 0.0f);
            player_give_effect(claimer, EFFECT_ROOTED, effect_max(game, EFFECT_ROOTED));
            player_give_effect(claimer, EFFECT_DIZZY, effect_max(game, EFFECT_DIZZY));
            break;
        case 16: {
            game_play_sound(game, "taunt_4", claimer->uid, claimer->position, 1.0f, 0.0f);
            Player *owner = find_player(game, d->owner_uid);
            const vec3 at = {d->position.x, d->position.y + 5.0f, d->position.z}; // y + heightS / 2
            game_explosion(game, at, 50.0f, 300.0f, owner ? owner : claimer, -1);
            break;
        }
        default: // KR / Junk / Skin: account rewards, no backend call yet (see the header)
            break;
    }
}

// AirDrop.claim(player): "rad", "krp owner, claimer", adrop_1, the package (player NULL: lost)
static void drop_claim(Game *game, AirDrop *d, Player *claimer) {
    if (!d->can_claim) return;
    d->active = 0;
    d->can_claim = 0;
    if (!game_is_authority(game)) return;
    drop_remove_send(game, d);
    if (!claimer) return;
    const NetAirDropClaimPacket krp = {.owner_uid = d->owner_uid, .claimer_uid = claimer->uid};
    game_send(game, NET_TARGET_ALL, NET_PACKET_AIRDROP_CLAIM, &krp, sizeof(krp));
    game_play_sound(game, "adrop_1", -1, d->position, 0.5f, 0.0f);
    package_claim(game, d, claimer);
}

void airdrop_tick(Game *game, const float delta_ms) {
    GameZombies *z = &game->zombies;
    const int authority = game_is_authority(game);
    for (int i = 0; i < z->drop_count; i++) {
        AirDrop *d = &z->drops[i];
        if (!d->active) continue;
        if (d->fall > 0.0f) {
            const float step = fminf(d->fall, AIRDROP_FALL_SPEED * delta_ms);
            d->fall -= step;
            d->position.y -= step;
            if (d->fall <= 0.0f) d->can_claim = 1; // readyClaim
        }
        if (authority && game->map && d->position.y <= game->map->death_y) { // claim() with no player
            d->can_claim = 1;
            drop_claim(game, d, NULL);
        }
    }
}

// Player.collides(drop, claimRadius): the drop's box is width / length / heightS 10 (halves)
static int drop_in_reach(const Player *p, const AirDrop *d) {
    const float r = 10.0f + AIRDROP_CLAIM_RADIUS;
    return p->position.x - p->scale < d->position.x + r && p->position.x + p->scale > d->position.x - r &&
           p->position.z - p->scale < d->position.z + r && p->position.z + p->scale > d->position.z - r &&
           p->position.y <= d->position.y + 10.0f && p->position.y + p->height >= d->position.y - 10.0f;
}

int airdrop_interact_prompt(const Game *game, const Player *player) {
    const GameZombies *z = &game->zombies;
    for (int i = 0; player && i < z->drop_count; i++) {
        if (z->drops[i].active && z->drops[i].can_claim && drop_in_reach(player, &z->drops[i])) return i;
    }
    return -1;
}

// player.js: hold interact on the ground; interProg += dt / claimTime, the claim at 1
void airdrop_player_collide(Player *player, const Input *input, const float delta) {
    Game *game = player->game;
    if (!game || !input) return;
    const int slot = airdrop_interact_prompt(game, player);
    if (slot < 0 || !input->interact_held || !player->on_ground) return;
    AirDrop *d = &game->zombies.drops[slot];
    player->making_progress = 1;
    player->interact_progress += delta * 1000.0f / (d->claim_time > 0.0f ? d->claim_time : 3250.0f);
    if (player->interact_progress < 1.0f) return;
    player->interact_progress = 0.0f;
    if (game_is_authority(game)) drop_claim(game, d, player); // game.claimAirDrop (serverIO only)
}

void airdrop_send_all(Game *game, const int uid) {
    const GameZombies *z = &game->zombies;
    const Player *viewer = find_player(game, uid);
    for (int i = 0; i < z->drop_count; i++) {
        if (z->drops[i].active) drop_send(game, &z->drops[i], uid, viewer && viewer->uid == z->drops[i].owner_uid);
    }
}

void airdrop_reset(Game *game) {
    memset(game->zombies.drops, 0, sizeof(game->zombies.drops));
    game->zombies.drop_count = 0;
}

// The client half of "ad" / "rad": cSync / removeBySid
int airdrop_apply_packet(Game *game, const int type, const void *payload, const uint16_t length) {
    GameZombies *z = &game->zombies;
    if (type == NET_PACKET_AIRDROP && length == sizeof(NetAirDropPacket)) {
        NetAirDropPacket packet;
        memcpy(&packet, payload, sizeof(packet));
        if (game_is_authority(game) || packet.sid < 1 || packet.sid > AIRDROP_MAX || packet.index >= AIRDROP_PACKAGE_COUNT) return 1;
        const int slot = packet.sid - 1;
        if (slot >= z->drop_count) z->drop_count = slot + 1;
        AirDrop *d = &z->drops[slot];
        memset(d, 0, sizeof(*d));
        d->sid = packet.sid;
        d->owner_uid = packet.owner_uid;
        d->position = (vec3) {packet.position[0], packet.position[1], packet.position[2]};
        d->fall = packet.fall;
        d->index = packet.index;
        d->imitate = -1;
        d->claim_time = g_airdrop_packages[packet.index].claim_time;
        d->active = 1;
        d->can_claim = packet.fall <= 0.0f;
        return 1;
    }
    if (type == NET_PACKET_AIRDROP_REMOVE && length == sizeof(NetAirDropRemovePacket)) {
        NetAirDropRemovePacket packet;
        memcpy(&packet, payload, sizeof(packet));
        if (!game_is_authority(game) && packet.sid >= 1 && packet.sid <= z->drop_count) {
            z->drops[packet.sid - 1].active = 0;
            z->drops[packet.sid - 1].can_claim = 0;
        }
        return 1;
    }
    return type == NET_PACKET_AIRDROP_CLAIM;
}
