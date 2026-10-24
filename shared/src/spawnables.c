// Krunker v7.1.4 spawnables (data/medals.js, the 25-entry table; game.js dropSpawnable /
// pickupSpawnable / shootSpawnable / updateSpawnables :2033-2184; doc/v7/map-logic.md §4.1c).
// Authority: drops, pickups (player.js collisions), despawn timers and the "crsp" / "dsp"
// packets (NET_PACKET_SPAWNABLE_V7). Clients mirror them into game->mode->spawnables.
//
// KN notes:
// - Account rewards: Frog and Krunkanium give their skin through game_give_skin (kn_brain, servers
//   with rewards only); Gift (event rarity tables) and Loot Drop (dropSkin, never dropped here)
//   pickups only remove the spawnable. Golden Egg needs the Egg Basket melee skin: nobody qualifies.
// - v7.1.4 bug fixed: a KR Tag / Diamond Egg stack picked up only partly (tagHoldLimit) was left
//   inactive but not destroyed (pickupSpawnable sets active = false before onPickup, dontDestroy
//   then skips the "dsp"), so the rest could never be collected and never despawned. KN keeps
//   the rest active and re-sends it with its new count.
// - Slime pads push on the authority (spawnables_update); clients take the velocity from the
//   reconciliation.
#include <spawnables.h>
#include <modes.h>
#include <ai.h>
#include <zombies.h> // map-logic WP-H: reserves, perks
#include <network_sounds.h>
#include <math.h>
#include <string.h>
#include "modes/mode_internal.h"

static const SpawnableInfo k_spawnables[SPAWNABLE_COUNT] = {
    [SPAWNABLE_TAG] = {.name = "Tag", .model = "tag_0", .scale = 6.0f, .pad_mlt = 0.8f, .y_off = 6.0f, .timer = 8.0f},
    [SPAWNABLE_GIFT] = {.name = "Gift", .model = "gift_0", .scale = 3.0f, .y_off = 6.0f, .timer = 20.0f},
    [SPAWNABLE_GOLDEN_EGG] = {.name = "Golden Egg", .model = "egg_0", .texture = "egg_0_4", .scale = 3.0f, .y_off = 1.0f, .timer = 40.0f, .no_anim = 1},
    [SPAWNABLE_KR_TAG] = {.name = "KR Tag", .model = "krunkie_0", .scale = 3.5f, .pad_mlt = 1.25f, .y_off = 6.0f, .timer = 12.0f},
    [SPAWNABLE_DIAMOND_EGG] = {.name = "Diamond Egg", .model = "egg_0", .texture = "egg_0_5", .scale = 2.0f, .y_off = 6.0f, .timer = 12.0f},
    [SPAWNABLE_SLIME_PAD] = {.name = "Slime Pad", .model = "pad_1", .scale = 4.0f, .y_off = 0.2f, .timer = 10.0f, .no_anim = 1, .always_active = 1},
    [SPAWNABLE_AMMO_PACK] = {.name = "Ammo Pack", .model = "ammobox_0", .scale = 1.5f, .y_off = 0.25f, .timer = 20.0f, .no_anim = 1},
    [SPAWNABLE_WEAPON_BOX] = {.name = "Weapon Box", .model = "weaponbox_0", .scale = 3.0f, .pad_mlt = 1.5f, .y_off = 0.9f, .timer = 20.0f, .no_anim = 1},
    [SPAWNABLE_MELEE_PICKUP] = {.name = "Melee Pickup", .model = "melee/melee_0", .scale = 3.0f, .y_off = 6.0f, .timer = 20.0f},
    [SPAWNABLE_AMMO_CACHE] = {.name = "Ammo Cache", .model = "ammobox_0", .scale = 4.5f, .y_off = 6.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_NUKE] = {.name = "Nuke", .model = "streak_0", .scale = 4.5f, .y_off = 6.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_MINIGUN] = {.name = "Minigun", .model = "weapons/weapon_26", .scale = 4.0f, .pad_mlt = 2500.0f, .y_off = 12.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_INSTA_KILL] = {.name = "Insta-kill", .model = "skull_0", .scale = 3.0f, .y_off = 8.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_DISCOUNT] = {.name = "Discount", .model = "sale_0", .scale = 2.0f, .pad_mlt = 1.5f, .y_off = 8.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_DOUBLE_POINTS] = {.name = "Double Points", .model = "doublepoints_0", .scale = 4.0f, .y_off = 8.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_GRENADE_LAUNCHER] = {.name = "Grenade Launcher", .model = "weapons/weapon_27", .scale = 2.0f, .pad_mlt = 2.0f, .y_off = 8.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_JOE_TAG] = {.name = "Joe Tag", .model = "tag_0", .scale = 6.0f, .pad_mlt = 0.8f, .y_off = 6.0f, .timer = 20.0f, .powerup = 1},
    [SPAWNABLE_FROG] = {.name = "Frog", .model = "collectibles/collect_19", .scale = 2.0f, .pad_mlt = 1.25f, .timer = 30.0f, .no_anim = 1},
    [SPAWNABLE_KRUNKANIUM] = {.name = "Krunkanium", .model = "collectibles/collect_11", .scale = 2.0f, .y_off = 6.0f, .timer = 30.0f},
    [SPAWNABLE_PERK_BOTTLE] = {.name = "Perk Bottle", .model = "bottle_1", .texture = "bottle_1_0", .scale = 2.0f, .pad_mlt = 2.0f, .y_off = 8.0f, .timer = 30.0f, .powerup = 1},
    [SPAWNABLE_ZOMBIE_PORTAL] = {.name = "Zombie Portal", .model = "zombie_portal_0", .scale = 2.0f, .pad_mlt = 8.0f, .y_off = -1.0f, .timer = 15.0f, .ai_only = 1},
    [SPAWNABLE_SKULL_EE] = {.name = "Skull EE", .model = "skull_0", .scale = 3.0f, .timer = 120.0f, .no_anim = 1, .powerup = 1, .can_shoot = 1},
    [SPAWNABLE_FREE_UPGRADE] = {.name = "Free Upgrade", .model = "krummachine_0", .scale = 2.0f, .y_off = 8.0f, .timer = 30.0f},
    [SPAWNABLE_MEGA_SLIME_PAD] = {.name = "Mega Slime Pad", .model = "pad_1", .scale = 20.0f, .y_off = 1.0f, .timer = 10.0f, .no_anim = 1, .always_active = 1},
    [SPAWNABLE_LOOT_DROP] = {.name = "Loot Drop", .model = "cardb_0", .scale = 4.0f, .pad_mlt = 1.5f, .y_off = 8.0f, .player_specific = 1},
};

const SpawnableInfo *spawnable_info(const int type) {
    return type >= 0 && type < SPAWNABLE_COUNT ? &k_spawnables[type] : NULL;
}

// getScale(data): the KR Tag / Diamond Egg grow 0.5 per tag; the Minigun model is tiny
static float spawnable_scale(const int type, const int c) {
    const SpawnableInfo *info = &k_spawnables[type];
    if (type == SPAWNABLE_KR_TAG || type == SPAWNABLE_DIAMOND_EGG) return info->scale + (float) (c ? c : 1) * 0.5f;
    if (type == SPAWNABLE_MINIGUN) return 0.0026568026773869323f;
    return info->scale;
}

static void spawnable_half(ModeSpawnable *spawnable) {
    const SpawnableInfo *info = &k_spawnables[spawnable->type];
    spawnable->half = spawnable_scale(spawnable->type, spawnable->data_c) * (info->pad_mlt > 0.0f ? info->pad_mlt : 1.0f);
}

void spawnables_reset(Game *game) {
    if (!game->mode) return;
    memset(game->mode->spawnables, 0, sizeof(game->mode->spawnables));
}

static int spawnable_slot(GameMode *mode) {
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) {
        if (!mode->spawnables[i].active) return i;
    }
    int slot = 0; // full: the one closest to despawning goes
    for (int i = 1; i < MODE_MAX_SPAWNABLES; i++) {
        if (mode->spawnables[i].timer < mode->spawnables[slot].timer) slot = i;
    }
    return slot;
}

// game.dropSpawnable(typeInd, owner, data, pos): at pos (else the owner), y + yOff, the owner's
// team; playerSpecific ones need a player owner and only it hears of them.
int spawnable_drop_ex(Game *game, const int type, Player *owner, const SpawnableData *data, const vec3 position) {
    if (!game || !game->mode || !game_is_authority(game) || type < 0 || type >= SPAWNABLE_COUNT) return -1;
    const SpawnableInfo *info = &k_spawnables[type];
    if (info->player_specific && !owner) return -1;

    GameMode *mode = game->mode;
    const int slot = spawnable_slot(mode);
    if (mode->spawnables[slot].active) { // replaced: its viewers drop it first
        mode->spawnables[slot].active = 0;
        mode_send_spawnable(game, slot);
    }

    ModeSpawnable *spawnable = &mode->spawnables[slot];
    *spawnable = (ModeSpawnable) {
        .sid = mode->next_spawnable_sid++,
        .type = type,
        .position = {position.x, position.y + info->y_off, position.z},
        .team = owner ? owner->team : 0,
        .timer = info->timer,
        .data_c = data ? data->c : 0,
        .data_w = data ? data->w : -1,
        .data_xd = data ? data->xd : 0.0f,
        .data_zd = data ? data->zd : 0.0f,
        .owner_uid = info->player_specific && owner ? owner->uid : -1,
        .active = 1,
    };
    spawnable_half(spawnable);
    mode_send_spawnable(game, slot);
    return spawnable->sid;
}

int spawnable_drop(Game *game, const int type, Player *owner, const int data, const vec3 position) {
    const SpawnableData d = {.c = data, .w = -1};
    return spawnable_drop_ex(game, type, owner, &d, position);
}

static void spawnable_destroy(Game *game, const int slot) {
    if (game->mode->spawnables[slot].type == SPAWNABLE_ZOMBIE_PORTAL) mode_zombies_portal_closed(game); // onDestroy (WP-H)
    game->mode->spawnables[slot].active = 0;
    mode_send_spawnable(game, slot); // "dsp sid"
}

int spawnable_alive(const Game *game, const int sid) {
    for (int i = 0; game && game->mode && sid >= 0 && i < MODE_MAX_SPAWNABLES; i++) {
        if (game->mode->spawnables[i].active && game->mode->spawnables[i].sid == sid) return 1;
    }
    return 0;
}

int spawnable_remove(Game *game, const int sid) {
    for (int i = 0; game && game->mode && sid >= 0 && i < MODE_MAX_SPAWNABLES; i++) {
        if (!game->mode->spawnables[i].active || game->mode->spawnables[i].sid != sid) continue;
        spawnable_destroy(game, i);
        return 1;
    }
    return 0;
}

// stopInteract(game, serverIO, player, spawnable): 1 = this player can't take it now
static int spawnable_stop(const Game *game, const Player *player, const ModeSpawnable *spawnable) {
    const GameModeConfig *config = &game->mode->config;
    switch (spawnable->type) {
        case SPAWNABLE_GIFT:
        case SPAWNABLE_AMMO_PACK:
        case SPAWNABLE_WEAPON_BOX:
        case SPAWNABLE_PERK_BOTTLE:
            return player->is_bot || player->perk_count >= ZOMBIES_PERK_COUNT; // WP-H: perks.length < zombiePerks.length
        case SPAWNABLE_GOLDEN_EGG:
            return 1; // Egg Basket melee skin + account egg list: not in KN
        case SPAWNABLE_KR_TAG:
        case SPAWNABLE_DIAMOND_EGG:
            return (!config->teams || player->team != spawnable->team) && !(player->mode.dog_tags < config->tag_hold_limit);
        case SPAWNABLE_MELEE_PICKUP:
            return player->is_bot || player->can_throw;
        case SPAWNABLE_MINIGUN:
        case SPAWNABLE_GRENADE_LAUNCHER:
            return player->is_bot || player_has_equipment(player); // WP-H: hasEquipment()
        case SPAWNABLE_FREE_UPGRADE: { // WP-H: !canUpgrade(): the held slot is base and upgradable
            const int s = player->loadout_index;
            const Weapon *w = player->loadout && s >= 0 && s < player->loadout_size && player->loadout[s] >= 0 &&
                              (size_t) player->loadout[s] < game->weapon_count ? game->weapons[player->loadout[s]] : NULL;
            return !w || !w->can_upgrade || (s < 8 && (player->upgraded_slots >> s & 1));
        }
        default:
            return 0;
    }
}

static void spawnable_refill_knife(Game *game, Player *player) {
    player->can_throw = 1; // refillPlayer
    game_send(game, player->uid, NET_PACKET_REFILL, NULL, 0);
}

// game.playSound(networkSounds.indexOf(name), 1.2, sid, x, y, z, true) of the weapon / ammo
// onPickups (medals.js:293, :332, :359, :384; deob-10.0.3.js:176815-176904): at the player, the
// player included. A name config.networkSounds lacks is id -1, which plays nothing: the melee
// pickup's "melee_equip" (:176880) is silent in 10.0.3 (shared/include/network_sounds.h).
static void spawnable_sound(Game *game, const Player *player, const char *name) {
    const char *sound = network_sound_name(network_sound_index(name));
    if (sound) game_play_sound(game, sound, player->uid, player->position, 1.2f, 0.0f);
}

// medals.js onPickup / onCollision of one spawnable for one player (authority)
static void spawnable_pickup(Game *game, Player *player, const int slot) {
    ModeSpawnable *spawnable = &game->mode->spawnables[slot];
    const GameModeConfig *config = &game->mode->config;
    const SpawnableInfo *info = &k_spawnables[spawnable->type];
    int keep = 0;

    switch (spawnable->type) {
        case SPAWNABLE_TAG: // Kill Confirmed: your team's tag denies (+10), the enemy's confirms (+1 team, +25)
            if (player->team == spawnable->team) {
                player->mode.denies++;
                mode_send_medal(game, player, "Denied", 10);
                player_score(player, 10, 2);
            } else {
                mode_team_score_add(game, player->team, 1);
                player->mode.confirms++;
                mode_send_medal(game, player, "Confirmed", 25);
                player_score(player, 25, 2);
            }
            break;
        case SPAWNABLE_KR_TAG:
        case SPAWNABLE_DIAMOND_EGG: {
            const int krt = spawnable->type == SPAWNABLE_KR_TAG;
            int count = spawnable->data_c;
            const int points = krt ? (config->teams ? 10 : 5) : 5;
            if (config->teams && player->team == spawnable->team) {
                player->mode.denies++;
                mode_send_medal(game, player, "Denied", points);
                player_score(player, points, 0);
                break;
            }
            if (!krt) count = spawnable->data_c; // Diamond Egg: data.c || 0
            const int limit = config->tag_hold_limit;
            if (player->mode.dog_tags + count > limit) {
                const int take = limit - player->mode.dog_tags;
                player->mode.dog_tags += take;
                spawnable->data_c = count - take;
                keep = spawnable->data_c > 0;
            } else {
                player->mode.dog_tags += count;
            }
            mode_send_medal(game, player, "Collected", points);
            mode_send_counter(game, NET_MODE_COUNTER_KR_TAGS, player, (float) player->mode.dog_tags, 1);
            player_score(player, points, 0);
            break;
        }
        case SPAWNABLE_AMMO_PACK: {
            int count = spawnable->data_c;
            if (count < 10) count = mode_rand_int(8, 12);
            if (player->ammo && player->loadout_size) player->ammo[0] += (unsigned int) count;
            mode_send_loadout(game, player); // "uamo"
            spawnable_sound(game, player, "reload_3");
            player_score(player, 5, 0);
            mode_send_medal(game, player, "Scavenger", 5);
            break;
        }
        case SPAWNABLE_WEAPON_BOX:
            player->mode.pickups++;
            if (spawnable->data_w >= 0) {
                mode_set_player_loadout(player, &spawnable->data_w, 1, 0); // replaceLoadout(player, [wId])
                mode_send_loadout(game, player);
                player_score(player, 5, 0);
                mode_send_medal(game, player, "Armed", 5);
            }
            spawnable_sound(game, player, "reload_3");
            break;
        case SPAWNABLE_MELEE_PICKUP:
            spawnable_refill_knife(game, player);
            spawnable_sound(game, player, "melee_equip");
            break;
        case SPAWNABLE_AMMO_CACHE: // every player: full ammo, knife back
            for (size_t i = 0; i < game->player_count; i++) {
                Player *other = game->players[i];
                if (!other || !other->ammo) continue;
                for (int s = 0; s < other->loadout_size; s++) {
                    const int id = other->loadout[s];
                    if (id >= 0 && (size_t) id < game->weapon_count && game->weapons[id]) other->ammo[s] = (unsigned int) mode_ammo_limit(game, (int) game->weapons[id]->ammo);
                }
                if (!other->can_throw) spawnable_refill_knife(game, other);
                mode_send_loadout(game, other);
                zombies_reserves_reset(other); // WP-H: resetAmmo(true, true) refills the reserves too
                if (zombies_finite_ammo(other)) zombies_send_reserves(other);
            }
            spawnable_sound(game, player, "reload_3");
            break;
        case SPAWNABLE_NUKE: // every non-ally AI dies, every player +400
            for (int i = game->ai ? game->ai->count - 1 : -1; i >= 0; --i) {
                GameAI *ai = &game->ai->ais[i];
#ifdef GAME_AI_TEAMS // WP-F: allied (team 1) AIs survive
                if (ai->team == 1) continue;
#endif
                if (ai->active) game_ai_kill(game, ai, NULL);
            }
            for (size_t i = 0; i < game->player_count; i++) {
                if (game->players[i]) player_score(game->players[i], 400, 0);
            }
            {
                const NetNukePacket nuke = {.state = 1}; // broadcast("n", 1): the detonation flash (WP-H)
                game_send(game, NET_TARGET_ALL, NET_PACKET_NUKE, &nuke, sizeof(nuke));
            }
            break;
        case SPAWNABLE_JOE_TAG:
            player_score(player, spawnable->data_c ? spawnable->data_c : 1, 0); // data.pnts || 1
            break;
        case SPAWNABLE_INSTA_KILL:
        case SPAWNABLE_DISCOUNT:
        case SPAWNABLE_DOUBLE_POINTS:
        case SPAWNABLE_MINIGUN:
        case SPAWNABLE_GRENADE_LAUNCHER:
        case SPAWNABLE_PERK_BOTTLE:
        case SPAWNABLE_FREE_UPGRADE:
            mode_zombies_spawnable(game, player, NULL, spawnable->type); // Zombies power-ups (WP-H)
            break;
        case SPAWNABLE_FROG: // onPickup: giveSkin(5366, "zombies")
            game_give_skin(game, player, 5366, "zombies");
            break;
        case SPAWNABLE_KRUNKANIUM: // onPickup: giveSkin(4222, "killdrops")
            game_give_skin(game, player, 4222, "killdrops");
            break;
        default: // Gift, Loot Drop: account rewards (see the header)
            break;
    }

    if (info->always_active) return;
    if (keep) { // KN fix (see the header): the rest of a partly taken stack stays
        spawnable_half(spawnable);
        mode_send_spawnable(game, slot);
        return;
    }
    spawnable_destroy(game, slot);
}

// Slime pads (onCollision): data.xD / zD tilt the push, flat pads push straight up (0.125, mega 0.225)
static void spawnable_pad(Player *player, const ModeSpawnable *spawnable) {
    const float power = spawnable->type == SPAWNABLE_MEGA_SLIME_PAD ? 0.225f : 0.125f;
    float xd = spawnable->data_xd, zd = spawnable->data_zd;
    const int up = (!xd && !zd) || zd == 180.0f;
    if (xd) xd -= 90.0f;
    if (!up) {
        const float xr = xd * (float) M_PI / 180.0f, zr = zd * (float) M_PI / 180.0f;
        const float vx = power * sinf(-zr) * cosf(xr), vz = power * cosf(-zr) * cosf(xr);
        if (vx) player->velocity.x = vx;
        if (vz) player->velocity.z = vz;
    } else {
        player->velocity.y = power * (zd == 180.0f ? -1.0f : 1.0f);
    }
    player->on_ground = 0;
}

static int spawnable_touches(const Player *player, const ModeSpawnable *spawnable) {
    const vec3 half = {spawnable->half, spawnable->half * 0.5f, spawnable->half};
    return mode_player_collides(player, spawnable->position, half);
}

// game.update #19 updateSpawnables (despawn timers) + the player.js spawnable collisions
void spawnables_update(Game *game, const float delta) {
    if (!game || !game->mode || !game_is_authority(game)) return;
    GameMode *mode = game->mode;

    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        ModeSpawnable *spawnable = &mode->spawnables[s];
        if (!spawnable->active) continue;
        const SpawnableInfo *info = &k_spawnables[spawnable->type];

        for (size_t i = 0; i < game->player_count && spawnable->active && !info->ai_only; i++) {
            Player *player = game->players[i];
            if (!player || !player->active || player->noclip || !spawnable_touches(player, spawnable)) continue;
            if (info->player_specific && player->uid != spawnable->owner_uid) continue;
            if (spawnable_stop(game, player, spawnable)) continue;
            if (spawnable->type == SPAWNABLE_SLIME_PAD || spawnable->type == SPAWNABLE_MEGA_SLIME_PAD) {
                spawnable_pad(player, spawnable);
                continue;
            }
            spawnable_pickup(game, player, s);
        }

        if (!spawnable->active || spawnable->timer <= 0.0f) continue;
        spawnable->timer -= delta;
        if (spawnable->timer <= 0.0f) spawnable_destroy(game, s);
    }
}

// game.shootSpawnable (canShoot spawnables: the Skull EE)
int spawnable_shoot(Game *game, Player *player, const int sid) {
    if (!game || !game->mode || !game_is_authority(game)) return 0;
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        ModeSpawnable *spawnable = &game->mode->spawnables[s];
        if (!spawnable->active || spawnable->sid != sid || !k_spawnables[spawnable->type].can_shoot) continue;
        mode_zombies_spawnable(game, player, NULL, spawnable->type); // Skull EE (WP-H)
        spawnable_destroy(game, s);
        return 1;
    }
    return 0;
}

// game.pickupSpawnable(ai, s) for aiOnly spawnables; the AI box is x/z +- scale, y .. y + 2 * scale.
int spawnable_pickup_ai(Game *game, GameAI *ai) {
    if (!game || !game->mode || !ai || !ai->active || !game_is_authority(game)) return 0;
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        ModeSpawnable *spawnable = &game->mode->spawnables[s];
        if (!spawnable->active || !k_spawnables[spawnable->type].ai_only) continue;
        const float h = spawnable->half * 0.5f;
        if (ai->position.x - ai->scale < spawnable->position.x + spawnable->half && ai->position.x + ai->scale > spawnable->position.x - spawnable->half &&
            ai->position.z - ai->scale < spawnable->position.z + spawnable->half && ai->position.z + ai->scale > spawnable->position.z - spawnable->half &&
            ai->position.y <= spawnable->position.y + h && ai->position.y + ai->scale * 2.0f >= spawnable->position.y - h) {
            mode_zombies_spawnable(game, NULL, ai, spawnable->type); // Zombie Portal (WP-H: eePAP order)
            spawnable_destroy(game, s);
            return 1;
        }
    }
    return 0;
}

// sync.spwns: every active spawnable to a joiner (playerSpecific ones to their owner only)
void spawnables_send_all(Game *game, const int uid) {
    if (!game || !game->mode) return;
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        const ModeSpawnable *spawnable = &game->mode->spawnables[s];
        if (!spawnable->active) continue;
        if (spawnable->owner_uid >= 0 && spawnable->owner_uid != uid) continue;
        NetSpawnableV7Packet packet;
        memset(&packet, 0, sizeof(packet));
        packet.sid = spawnable->sid;
        packet.type = (uint8_t) spawnable->type;
        packet.active = 1;
        packet.team = spawnable->team;
        packet.position[0] = spawnable->position.x;
        packet.position[1] = spawnable->position.y;
        packet.position[2] = spawnable->position.z;
        packet.data_c = spawnable->data_c;
        packet.data_w = spawnable->data_w;
        packet.data_xd = spawnable->data_xd;
        packet.data_zd = spawnable->data_zd;
        game_send(game, uid, NET_PACKET_SPAWNABLE_V7, &packet, sizeof(packet));
    }
}

// ------------------------------------------------------------------ mode hooks for other packages

// mode.collisionCheck runs from mode_tick for every active player (king / dom set onZone); the
// WP-B collision chain needs nothing from the mode per object.
int mode_v7_collision_check(Game *game, Player *player, const Object *object) {
    (void) game;
    (void) player;
    (void) object;
    return 0;
}

// trigger action 71 "Mode Specific": mode.onTrigger (Zombies: the portal)
void mode_v7_on_trigger(Game *game, Player *player, const int value) {
    if (!game || !game->mode || !game_is_authority(game)) return;
    if (game->mode->config.id == MODE_ID_ZOM) mode_zombies_on_trigger(game, player, value);
}

// mode.onAIDeath (AI.kill): Zombies drops and waves; v10.0.3 Raid's "aik" challenge (deob-10.0.3.js:118440-118447)
void mode_v7_on_ai_death(Game *game, GameAI *ai, Player *killer) {
    if (!game || !game->mode || !ai || !game_is_authority(game)) return;
    if (game->mode->config.id == MODE_ID_ZOM) mode_zombies_on_ai_death(game, ai, killer);
    else if (game->mode->config.id == MODE_ID_RAID) mode_ai_kill_challenge(game, killer);
}

// kc.js onKill: dropSpawnable(0, victim): a tag of the victim's team where it died
void mode_drop_spawnable(Game *game, const Player *victim) {
    if (victim) spawnable_drop(game, SPAWNABLE_TAG, (Player *) victim, 0, victim->position);
}
