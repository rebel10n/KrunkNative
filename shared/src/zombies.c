// Krunker v7.1.4 zombies machines, power-ups and finiteAmmo reserves (map-logic WP-H stage 2,
// doc/v7/map-logic.md §2.7, §4.1b; shared/include/zombies.h). The machines are map objects and
// work in any mode, as in v7: mystery boxes (game.js :2201-2419, players.js interact 13), perk
// machines (interact 14, player.js gotFreeP), krum machines (game.js :2420-2588, interact 15).
//
// v7.1.4 bugs fixed here (doc/v7/decisions.md "v7 bugs"):
// - updateMysteryBoxes / updateKrumMachines end a cycle with `pendingWeapon == null` (a
//   comparison): the old weapon stays pending. Here it is cleared with the cycle.
#include <zombies.h>
#include <spawnables.h>
#include <ai.h>
#include "modes/mode_internal.h"
#include <math.h>
#include <string.h>

const char *const g_zombie_perk_names[ZOMBIES_PERK_COUNT] = {
    "Krum", "Double-Up", "Western Hands", "Swift", "Power Slide", "Drift Fuze", "Holster",
};

// weapons.js mysteryBoxR by weapon id (-1: not in the box: knife, bomb, tools, streak weapons)
static const signed char k_mystery_box_rarity[] = {
    1, 2, 0, 2, 1, 3, 3, 2, 0, 1, 0, 1, -1, 1, 2, 0, 1, -1, 1, -1, -1, 0, 0, -1, 0, -1, -1, 0, 4,
};
static const float k_mystery_box_rarities[] = {35.0f, 30.0f, 20.0f, 10.0f, 5.0f}; // weapons.mysteryBoxRarities

static float zrand_percent(void) {
    return roundf(mode_random() * 10000.0f) / 100.0f; // UTILS.randFloat(0, 100).round(2)
}

static int waiting(const Game *game) {
    // !game.waitTimers || game.waitTimers[0].contTime
    return game->mode && game->mode->wait_timer_count && !game->mode->wait_timers[0].cont_time;
}

static const Weapon *weapon_by_id(const Game *game, const int id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : g_weapons;
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

static Object *object_at(const Game *game, const int uid) {
    return game->map && uid >= 0 && (size_t) uid < game->map->object_count ? game->map->objects[uid] : NULL;
}

// Player.collides(obj, tRadius): the object box padded by the radius on x / z
static int machine_in_range(const Player *p, const Object *o) {
    const float hx = o->scale.x * 0.5f + ZOMBIES_MACHINE_RADIUS, hz = o->scale.z * 0.5f + ZOMBIES_MACHINE_RADIUS, hy = o->scale.y * 0.5f;
    const float cx = o->position.x, cy = o->position.y + o->scale.y * 0.5f, cz = o->position.z;
    return p->position.x - p->scale < cx + hx && p->position.x + p->scale > cx - hx && p->position.z - p->scale < cz + hz &&
           p->position.z + p->scale > cz - hz && p->position.y <= cy + hy && p->position.y + p->height >= cy - hy;
}

static int machine_price(const Object *o) {
    return o->score_points ? (int) o->score_points : 1; // scoreP: parseInt(sp) || 0, then || 1
}

static ZombieMachine *find_machine(Game *game, const int uid, int *kind) {
    GameZombies *z = &game->zombies;
    for (int i = 0; i < z->box_count; i++) {
        if (z->boxes[i].uid != uid) continue;
        if (kind) *kind = ZOMBIE_MACHINE_MYSTERY_BOX;
        return &z->boxes[i];
    }
    for (int i = 0; i < z->krum_count; i++) {
        if (z->krums[i].uid != uid) continue;
        if (kind) *kind = ZOMBIE_MACHINE_KRUM;
        return &z->krums[i];
    }
    return NULL;
}

const ZombieMachine *zombies_machine(const Game *game, const int uid) {
    return find_machine((Game *) game, uid, NULL);
}

static void machine_clear(ZombieMachine *m) {
    m->state = 0;
    m->timer = 0.0f;
    m->pending_weapon = ZOMBIES_NONE;
    m->purchased_by = ZOMBIES_NONE;
}

// syncMysteryBox / syncKrumMachine
static void machine_sync(Game *game, const ZombieMachine *m, const int kind, const int uid) {
    const Object *o = object_at(game, m->uid);
    const NetZombieMachinePacket packet = {
        .kind = (uint8_t) kind,
        .active = (uint8_t) (o && o->active),
        .state = (uint8_t) m->state,
        .uid = m->uid,
        .timer = m->timer,
        .pending_weapon = m->pending_weapon,
        .purchased_by = m->purchased_by,
    };
    game_send(game, uid, NET_PACKET_ZOMBIE_MACHINE, &packet, sizeof(packet));
}

// ------------------------------------------------------------------ power-ups

int zombies_insta_kill(const Game *game) {
    return game && game->zombies.powerups[ZOMBIES_POWERUP_INSTA_KILL] > 0.0f;
}

int zombies_double_points(const Game *game) {
    return game && game->zombies.powerups[ZOMBIES_POWERUP_DOUBLE_POINTS] > 0.0f;
}

static void powerup_send(Game *game, const int index, const int uid) {
    const NetPowerupPacket packet = {.index = (uint8_t) index, .ms = (int32_t) game->zombies.powerups[index]};
    game_send(game, uid, NET_PACKET_POWERUP, &packet, sizeof(packet));
}

// medals.js onPickup: instaKillTimer 30000 / fireSaleTimer 20000 (+ every box on) / doublePointsTimer 30000
void zombies_set_powerup(Game *game, const int index, const float ms) {
    if (!game || index < 0 || index >= ZOMBIES_POWERUPS) return;
    game->zombies.powerups[index] = ms;
    if (!game_is_authority(game)) return;
    powerup_send(game, index, NET_TARGET_ALL);
    if (index == ZOMBIES_POWERUP_FIRE_SALE && ms > 0.0f) zombies_switch_mystery_box(game, zombies_active_mystery_box(game), 1);
}

// game.updatePowerups
static void powerups_tick(Game *game, const float delta_ms) {
    const int authority = game_is_authority(game);
    for (int i = 0; i < ZOMBIES_POWERUPS; i++) {
        float *timer = &game->zombies.powerups[i];
        if (*timer <= 0.0f) continue;
        *timer -= delta_ms;
        if (*timer > 0.0f) continue;
        *timer = 0.0f;
        if (!authority) continue;
        powerup_send(game, i, NET_TARGET_ALL);
        if (i == ZOMBIES_POWERUP_FIRE_SALE) zombies_switch_mystery_box(game, zombies_active_mystery_box(game), 0);
    }
}

// ------------------------------------------------------------------ mystery boxes

int zombies_mystery_box_price(const Game *game, const Player *player, const Object *box) {
    if (game->zombies.powerups[ZOMBIES_POWERUP_FIRE_SALE] > 0.0f) return ZOMBIES_FIRE_SALE_PRICE;
    const int price = machine_price(box);
    return player->sharing_is_caring ? (price + 1) / 2 : price; // Math.ceil(scoreP / 2)
}

int zombies_active_mystery_box(const Game *game) {
    for (int i = 0; i < game->zombies.box_count; i++) {
        const Object *o = object_at(game, game->zombies.boxes[i].uid);
        if (o && o->active) return game->zombies.boxes[i].uid;
    }
    return -1;
}

// chooseMysteryBox: any other box than the active one (when there is more than one)
static int choose_mystery_box(const Game *game) {
    const GameZombies *z = &game->zombies;
    if (!z->box_count) return -1;
    if (z->box_count == 1) return z->boxes[0].uid;
    const int active = zombies_active_mystery_box(game);
    int options[ZOMBIES_MAX_MACHINES], count = 0;
    for (int i = 0; i < z->box_count; i++) {
        if (z->boxes[i].uid != active) options[count++] = z->boxes[i].uid;
    }
    return count ? options[mode_rand_int(0, count - 1)] : -1;
}

void zombies_switch_mystery_box(Game *game, int uid, const int enable) {
    GameZombies *z = &game->zombies;
    if (!z->box_count) return;
    int picked = 0;
    if (uid < 0) {
        picked = 1;
        uid = choose_mystery_box(game);
    }
    for (int i = 0; i < z->box_count; i++) {
        ZombieMachine *m = &z->boxes[i];
        Object *o = object_at(game, m->uid);
        if (!o) continue;
        o->active = m->uid == uid || enable;
        if ((m->uid != uid && enable) || picked) machine_clear(m);
    }
    if (!game_is_authority(game)) return;
    const NetMysteryBoxSwitchPacket packet = {.uid = uid, .enable = (uint8_t) !!enable};
    game_send(game, NET_TARGET_ALL, NET_PACKET_MYSTERY_BOX_SWITCH, &packet, sizeof(packet)); // "smybx"
}

int zombies_mystery_box_weapon(const Game *game, const int teddy_chance) {
    if (teddy_chance && mode_rand_int(0, 100) <= teddy_chance && !(game->zombies.powerups[ZOMBIES_POWERUP_FIRE_SALE] > 0.0f)) return -1;
    float roll = zrand_percent();
    const int tiers = (int) (sizeof(k_mystery_box_rarities) / sizeof(k_mystery_box_rarities[0]));
    if (k_mystery_box_rarities[0] == 0.0f) roll = fmaxf(roll, 0.01f);
    int tier = 0;
    float sum = 0.0f;
    for (int i = 0; i < tiers; i++) {
        sum += k_mystery_box_rarities[i];
        if (roll <= sum) {
            tier = i;
            break;
        }
    }
    int options[64], count = 0;
    const int known = (int) sizeof(k_mystery_box_rarity);
    for (int id = 0; id < known && count < 64; id++) {
        if (k_mystery_box_rarity[id] == tier && weapon_by_id(game, id)) options[count++] = id;
    }
    return count ? options[mode_rand_int(0, count - 1)] : -1;
}

// game.updateMysteryBoxes (server half): rolling 5 s -> collect 15 s (teddy 8 s) -> idle; a
// teddy moves the box (another one during a fire sale).
static void boxes_tick(Game *game, const float delta_ms) {
    GameZombies *z = &game->zombies;
    const int fire_sale = z->powerups[ZOMBIES_POWERUP_FIRE_SALE] > 0.0f;
    for (int i = 0; i < z->box_count; i++) {
        ZombieMachine *m = &z->boxes[i];
        const Object *o = object_at(game, m->uid);
        if (!o || !(o->active || fire_sale) || m->state < 1) continue;
        m->timer -= delta_ms;
        if (m->timer > 0.0f) continue;
        m->state++;
        m->timer = m->state == 2 ? (m->pending_weapon == -1 ? ZOMBIES_MYSTERY_BOX_DESPAWN_TEDDY : ZOMBIES_MYSTERY_BOX_DESPAWN) : ZOMBIES_MYSTERY_BOX_ANIM;
        if (m->state >= 3) {
            const int teddy = m->pending_weapon == -1;
            machine_clear(m);
            if (teddy) zombies_switch_mystery_box(game, fire_sale ? choose_mystery_box(game) : -1, fire_sale);
        }
        machine_sync(game, m, ZOMBIE_MACHINE_MYSTERY_BOX, NET_TARGET_ALL);
    }
}

// game.updateKrumMachines (server half): upgrading 5 s -> collect 15 s -> idle
static void krums_tick(Game *game, const float delta_ms) {
    GameZombies *z = &game->zombies;
    for (int i = 0; i < z->krum_count; i++) {
        ZombieMachine *m = &z->krums[i];
        const Object *o = object_at(game, m->uid);
        if (!o || !o->active || m->state < 1) continue;
        m->timer -= delta_ms;
        if (m->timer > 0.0f) continue;
        m->state++;
        m->timer = m->state == 2 ? ZOMBIES_KRUM_DESPAWN : ZOMBIES_KRUM_ANIM;
        if (m->state >= 3) machine_clear(m);
        machine_sync(game, m, ZOMBIE_MACHINE_KRUM, NET_TARGET_ALL);
    }
}

// Client half: the timers run between packets (the renderer reads state / timer).
static void machines_client_tick(Game *game, const float delta_ms) {
    GameZombies *z = &game->zombies;
    for (int k = 0; k < 2; k++) {
        ZombieMachine *list = k ? z->krums : z->boxes;
        const int count = k ? z->krum_count : z->box_count;
        for (int i = 0; i < count; i++) {
            ZombieMachine *m = &list[i];
            if (m->state < 1) continue;
            m->timer -= delta_ms;
            if (m->timer > 0.0f) continue;
            m->state++;
            m->timer = m->state == 2 ? (k ? ZOMBIES_KRUM_DESPAWN : m->pending_weapon == -1 ? ZOMBIES_MYSTERY_BOX_DESPAWN_TEDDY : ZOMBIES_MYSTERY_BOX_DESPAWN)
                                     : (k ? ZOMBIES_KRUM_ANIM : ZOMBIES_MYSTERY_BOX_ANIM);
            if (m->state >= 3) machine_clear(m);
        }
    }
}

// ------------------------------------------------------------------ weapons

static int weapon_kind_equipment(const Weapon *w) {
    return w && w->equipment;
}

// players.giveWeapon(player, id, upgraded, true): an owned weapon is refilled (and upgraded or
// not); equipment goes first; with a full loadout (2 guns, 3 with perk 6) the held gun is
// replaced (the first slot when the knife is in hand); otherwise it goes before the knife.
void zombies_give_weapon(Player *player, const int weapon_id, const int upgraded) {
    Game *game = player ? player->game : NULL;
    if (!game || !player->loadout || !game_is_authority(game)) return;
    const Weapon *weapon = weapon_by_id(game, weapon_id);
    if (!weapon || player_has_equipment(player)) return;

    int ids[NET_LOADOUT_MAX + 1];
    int count = 0;
    for (int i = 0; i < player_loadout_base(player) && count < NET_LOADOUT_MAX; i++) ids[count++] = player->loadout[i];
    unsigned int old_ammo[NET_LOADOUT_MAX + 1];
    int old_ids[NET_LOADOUT_MAX + 1];
    const int old_count = count;
    const unsigned char old_upgrades = player->upgraded_slots;
    for (int i = 0; i < count; i++) {
        old_ids[i] = ids[i];
        old_ammo[i] = player->ammo[i];
    }

    int slot = -1;
    for (int i = 0; i < count; i++) {
        if (ids[i] == weapon_id) slot = i;
    }
    if (slot < 0) {
        const int limit = player_has_perk(player, PERK_WEAPON_SLOT) ? 3 : 2;
        int knife = -1;
        for (int i = 0; i < count; i++) {
            if (ids[i] == WEAPON_ID_KNIFE) knife = i;
        }
        if (weapon_kind_equipment(weapon) || count <= limit) {
            const int at = weapon_kind_equipment(weapon) ? 0 : knife < 0 ? (count ? count - 1 : 0) : knife;
            if (count >= NET_LOADOUT_MAX) return;
            for (int i = count; i > at; i--) ids[i] = ids[i - 1];
            ids[at] = weapon_id;
            count++;
            slot = at;
        } else {
            slot = player->loadout_index == knife ? 0 : player->loadout_index;
            ids[slot] = weapon_id;
        }
    }

    // updateLoadout: kept weapons keep their ammo and upgrade; the new one is fresh
    mode_set_player_loadout(player, ids, count, 0);
    unsigned char upgrades = 0;
    for (int i = 0; i < player->loadout_size; i++) {
        if (i == slot) continue;
        for (int k = 0; k < old_count; k++) {
            if (old_ids[k] != player->loadout[i]) continue;
            player->ammo[i] = old_ammo[k];
            if (old_upgrades >> k & 1) upgrades |= (unsigned char) (1u << i);
            old_ids[k] = -1;
            break;
        }
    }
    player_set_upgrades(player, upgrades);
    if (slot >= player->loadout_size) slot = player->loadout_size - 1;
    player_swap_weapon(player, slot, 1, 0, 0);
    mode_send_loadout(game, player);
    if (upgraded) player_upgrade_weapon(player, slot); // upgradeWeapons entry, its full ammo, UPGRADES
    else if (upgrades) player_send_upgrades(player);   // the kept bits (LOADOUT cleared them client-side)
    for (int i = 0; i < player->reserve_count; i++) {  // the new weapon's reserve: full mags
        if (player->reserve_weapons[i] != weapon_id) continue;
        player->reserve_weapons[i] = player->reserve_weapons[--player->reserve_count];
        player->reserve_ammo[i] = player->reserve_ammo[player->reserve_count];
        break;
    }
    if (zombies_finite_ammo(player)) {
        player_reserve_ammo(player, slot);
        zombies_send_reserves(player);
    }
}

// ------------------------------------------------------------------ finiteAmmo

int zombies_finite_ammo(const Player *player) {
    return player && player->game && player->game->mode && player->game->mode->config.finite_ammo && !player->is_bot;
}

static int weapon_reserve_full(const Game *game, const Weapon *weapon) {
    const int mags = weapon->mag_reserve ? weapon->mag_reserve : 1; // getReserveMags: magReserve ?? 1
    return mode_ammo_limit(game, (int) weapon->ammo) * mags;
}

int player_reserve_ammo(Player *player, const int slot) {
    if (!player || !player->loadout || slot < 0 || slot >= player->loadout_size) return 0;
    const int id = player->loadout[slot];
    for (int i = 0; i < player->reserve_count; i++) {
        if (player->reserve_weapons[i] == id) return player->reserve_ammo[i];
    }
    const Weapon *weapon = weapon_by_id(player->game, id);
    const int full = weapon ? weapon_reserve_full(player->game, weapon) : 0;
    // a weapon new to the loadout: drop the reserves of weapons no longer held, then add it
    for (int i = player->reserve_count - 1; i >= 0; i--) {
        int held = 0;
        for (int s = 0; s < player->loadout_size; s++) held |= player->loadout[s] == player->reserve_weapons[i];
        if (held) continue;
        player->reserve_weapons[i] = player->reserve_weapons[--player->reserve_count];
        player->reserve_ammo[i] = player->reserve_ammo[player->reserve_count];
    }
    if (player->reserve_count < PLAYER_RESERVE_MAX) {
        player->reserve_weapons[player->reserve_count] = id;
        player->reserve_ammo[player->reserve_count++] = full;
    }
    return full;
}

void player_set_reserve_ammo(Player *player, const int slot, const int value) {
    if (!player || !player->loadout || slot < 0 || slot >= player->loadout_size) return;
    player_reserve_ammo(player, slot); // make sure it exists
    for (int i = 0; i < player->reserve_count; i++) {
        if (player->reserve_weapons[i] == player->loadout[slot]) player->reserve_ammo[i] = value;
    }
}

void zombies_reserves_reset(Player *player) {
    if (!player) return;
    player->reserve_count = 0;
    for (int s = 0; player->loadout && s < player->loadout_size; s++) player_reserve_ammo(player, s);
}

void zombies_send_reserves(Player *player) {
    Game *game = player->game;
    if (!game || !game_is_authority(game)) return;
    NetReserveAmmoPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.count = (uint8_t) player->reserve_count;
    for (int i = 0; i < player->reserve_count; i++) {
        packet.weapons[i] = player->reserve_weapons[i];
        packet.ammo[i] = player->reserve_ammo[i];
    }
    game_send(game, player->uid, NET_PACKET_RESERVE_AMMO, &packet, sizeof(packet));
}

int zombies_reload_allowed(Player *player) {
    return !zombies_finite_ammo(player) || player_reserve_ammo(player, player->loadout_index) > 0;
}

// player.js reload end with finiteAmmo: refill the magazine from the reserve
void zombies_reload_finish(Player *player) {
    const int slot = player->loadout_index;
    const int limit = mode_ammo_limit(player->game, (int) player->weapon->ammo);
    int reserve = player_reserve_ammo(player, slot) + (int) player->ammo[slot] - limit;
    int mag = limit;
    if (reserve < 0) {
        mag = limit + reserve;
        reserve = 0;
    }
    player_set_reserve_ammo(player, slot, reserve);
    player->ammo[slot] = (unsigned int) (mag > 0 ? mag : 0);
    zombies_send_reserves(player);
}

// ------------------------------------------------------------------ interaction

static int krum_offers(const Player *player, const ZombieMachine *m) {
    const int slot = player->loadout_index;
    const Weapon *base = player->loadout && slot >= 0 && slot < player->loadout_size ? weapon_by_id(player->game, player->loadout[slot]) : NULL;
    const int upgraded = slot >= 0 && slot < 8 && (player->upgraded_slots >> slot & 1);
    const int mine = m->purchased_by == player->uid;
    // player.js: the krum prompt unless ((upgraded || !canUpgrade) && not mine) || equipment ||
    // upgrading || someone else's
    if (((upgraded || !base || !base->can_upgrade) && !mine) || player_has_equipment(player) || m->state == 1 ||
        (m->purchased_by != ZOMBIES_NONE && !mine)) return 0;
    return 1;
}

int zombies_interact_prompt(const Game *game, const Player *player, const Object *object, const int uid) {
    if (!object || !(object->prefab == PREFAB_MYSTERY_BOX || object->prefab == PREFAB_PERK_MACHINE || object->prefab == PREFAB_KRUM_MACHINE)) {
        return GAME_INTERACT_NONE;
    }
    if (!object->active || !machine_in_range(player, object)) return GAME_INTERACT_NONE;
    if (object->team && game->mode && game->mode->config.teams && (int) object->team != player->team) return GAME_INTERACT_NONE;
    if (object->prefab == PREFAB_PERK_MACHINE) return player_has_perk(player, object->zombie_perk) ? GAME_INTERACT_NONE : GAME_INTERACT_PERK;
    const ZombieMachine *m = zombies_machine(game, uid);
    if (!m) return GAME_INTERACT_NONE;
    if (object->prefab == PREFAB_KRUM_MACHINE) return krum_offers(player, m) ? GAME_INTERACT_KRUM : GAME_INTERACT_NONE;
    if (player_has_equipment(player) || m->state == 1 || m->pending_weapon == -1 ||
        (m->purchased_by != ZOMBIES_NONE && m->purchased_by != -1 && m->purchased_by != player->uid)) return GAME_INTERACT_NONE;
    return GAME_INTERACT_MYSTERY_BOX;
}

static void purchase(Game *game, Player *player, const int price) {
    mode_send_medal(game, player, "Purchased", -price); // "am" ["Purchased", -price, 0, 1]
    player_score(player, -price, 0);
}

static void use_mystery_box(Game *game, Player *player, ZombieMachine *m, const Object *box) {
    if (m->state == 2) {
        if ((m->purchased_by == -1 || m->purchased_by == player->uid) && m->pending_weapon != -1 && m->pending_weapon != ZOMBIES_NONE) {
            zombies_give_weapon(player, m->pending_weapon, 0);
            machine_clear(m);
            machine_sync(game, m, ZOMBIE_MACHINE_MYSTERY_BOX, NET_TARGET_ALL);
        }
        return;
    }
    const int price = zombies_mystery_box_price(game, player, box);
    if (m->state != 0 || player->score < price) return;
    m->purchased_by = player->uid;
    m->pending_weapon = zombies_mystery_box_weapon(game, player->sharing_is_caring ? 0 : ZOMBIES_TEDDY_CHANCE);
    m->timer = ZOMBIES_MYSTERY_BOX_ANIM;
    m->state = 1;
    if (m->pending_weapon != -1) purchase(game, player, price);
    game_play_sound(game, m->pending_weapon == -1 ? "mystery_1" : "mystery_0", -1, box->position, 0.5f, 0.0f);
    machine_sync(game, m, ZOMBIE_MACHINE_MYSTERY_BOX, NET_TARGET_ALL);
}

static void use_krum_machine(Game *game, Player *player, ZombieMachine *m, const Object *machine) {
    if (m->state == 2) {
        if (m->purchased_by == -1 || m->purchased_by == player->uid) {
            zombies_give_weapon(player, m->pending_weapon, 1);
            machine_clear(m);
            machine_sync(game, m, ZOMBIE_MACHINE_KRUM, NET_TARGET_ALL);
        }
        return;
    }
    const int slot = player->loadout_index;
    const Weapon *base = player->loadout && slot >= 0 && slot < player->loadout_size ? weapon_by_id(game, player->loadout[slot]) : NULL;
    const int can = base && base->can_upgrade && !(slot < 8 && (player->upgraded_slots >> slot & 1));
    const int price = machine_price(machine);
    if (m->state != 0 || player->score < price || !can) return;
    m->purchased_by = player->uid;
    m->pending_weapon = player->loadout[slot];
    m->timer = ZOMBIES_MYSTERY_BOX_ANIM; // v7 uses the box's animation time here too (the same 5 s)
    m->state = 1;
    player_remove_weapon(player, m->pending_weapon);
    purchase(game, player, price);
    game_play_sound(game, "mystery_0", -1, machine->position, 0.5f, 0.0f);
    machine_sync(game, m, ZOMBIE_MACHINE_KRUM, NET_TARGET_ALL);
}

static void use_perk_machine(Game *game, Player *player, const Object *machine) {
    if (player->perk_count >= GAME_MAX_ZOMBIE_PERKS) return;
    const int price = machine_price(machine);
    if (player->score < price || player_has_perk(player, machine->zombie_perk)) return;
    player_give_perk(player, machine->zombie_perk);
    purchase(game, player, price);
    game_play_sound(game, "asset:30431", -1, machine->position, 0.5f, 0.0f); // playSound(30431, 0.5, ...) at the machine
    game_play_sound(game, "asset:30220", -1, player->position, 2.0f, 0.0f);  // and 30220 at the player
}

// players.interact types 13 / 14 / 15 ("int" type, uid): the machine must still offer that
// interaction to the player (range, team, state), and no waitTimer may hold the clock
void zombies_player_interact(Game *game, Player *player, const int type, const int uid) {
    if (!game || !player || !game->map || !game_is_authority(game) || waiting(game)) return;
    const Object *o = object_at(game, uid);
    if (!o || zombies_interact_prompt(game, player, o, uid) != type) return;
    if (type == GAME_INTERACT_PERK) {
        use_perk_machine(game, player, o);
        return;
    }
    ZombieMachine *m = find_machine(game, uid, NULL);
    if (!m) return;
    if (type == GAME_INTERACT_MYSTERY_BOX) use_mystery_box(game, player, m, o);
    else use_krum_machine(game, player, m, o);
}

void zombies_perk_machine_touch(Game *game, Player *player, const int uid) {
    if (!game_is_authority(game) || player->crouch_val != 1.0f) return;
    GameZombies *z = &game->zombies;
    for (int i = 0; i < z->perk_machine_count; i++) {
        if (z->perk_machines[i] != uid || z->perk_free_taken[i]) continue;
        z->perk_free_taken[i] = 1;
        player_score(player, 50, 0);
        return;
    }
}

// players.js melee: hitting the box holding your own weapon shares it (5 shares: sharingIsCaring)
void zombies_melee_object(Game *game, Player *player, const Object *object) {
    if (!game || !object || object->prefab != PREFAB_MYSTERY_BOX || !game_is_authority(game)) return;
    for (int i = 0; i < game->zombies.box_count; i++) {
        ZombieMachine *m = &game->zombies.boxes[i];
        if (object_at(game, m->uid) != object) continue;
        if (m->purchased_by != player->uid || m->state != 2 || m->pending_weapon == -1 || m->pending_weapon == ZOMBIES_NONE) return;
        if (!(game->zombies.powerups[ZOMBIES_POWERUP_FIRE_SALE] > 0.0f) && ++player->shared_boxes == 5) {
            player->sharing_is_caring = 1;
            const NetSharingPacket packet = {.on = 1};
            game_send(game, player->uid, NET_PACKET_SHARING, &packet, sizeof(packet)); // "sic"
        }
        m->purchased_by = -1;
        game_play_sound(game, "bhop_reset", player->uid, player->position, 1.5f, 0.0f);
        machine_sync(game, m, ZOMBIE_MACHINE_MYSTERY_BOX, NET_TARGET_ALL);
        return;
    }
}

// players.js shoot: a canShoot spawnable (the Skull EE) the bullet reaches before a wall is shot
// (game.shootSpawnable), while the clock runs
void zombies_shoot_spawnables(Game *game, Player *player, const vec3 origin, const vec3 dir, const float stop_t) {
    if (!game || !game->mode || !game_is_authority(game) || waiting(game)) return;
    int best = -1;
    float best_t = stop_t;
    for (int s = 0; s < MODE_MAX_SPAWNABLES; s++) {
        const ModeSpawnable *spawnable = &game->mode->spawnables[s];
        const SpawnableInfo *info = spawnable->active ? spawnable_info(spawnable->type) : NULL;
        if (!info || !info->can_shoot) continue;
        const float h = spawnable->half * 0.5f;
        const vec3 base = {spawnable->position.x, spawnable->position.y - h, spawnable->position.z};
        const vec3 size = {spawnable->half * 2.0f, h * 2.0f, spawnable->half * 2.0f};
        const float t = line_in_rect(origin, dir, base, size);
        if (t > 0.0f && t < best_t) {
            best_t = t;
            best = s;
        }
    }
    if (best >= 0) spawnable_shoot(game, player, game->mode->spawnables[best].sid);
}

// ------------------------------------------------------------------ lifecycle

void zombies_round_start(Game *game) {
    GameZombies *z = &game->zombies;
    memset(z->powerups, 0, sizeof(z->powerups));
    z->box_count = z->krum_count = z->perk_machine_count = 0;
    z->wave = 0;
    z->special[0] = 0;
    for (size_t i = 0; game->map && i < game->map->object_count; i++) {
        Object *o = game->map->objects[i];
        if (o->prefab == PREFAB_MYSTERY_BOX && z->box_count < ZOMBIES_MAX_MACHINES) {
            ZombieMachine *m = &z->boxes[z->box_count++];
            m->uid = (int) i;
            machine_clear(m);
            o->active = 0; // resetMysteryBoxes
        } else if (o->prefab == PREFAB_KRUM_MACHINE && z->krum_count < ZOMBIES_MAX_MACHINES) {
            ZombieMachine *m = &z->krums[z->krum_count++];
            m->uid = (int) i;
            machine_clear(m);
            o->active = 1; // resetKrumMachines
        } else if (o->prefab == PREFAB_PERK_MACHINE && z->perk_machine_count < ZOMBIES_MAX_MACHINES) {
            z->perk_free_taken[z->perk_machine_count] = 0;
            z->perk_machines[z->perk_machine_count++] = (int) i;
        }
    }
    airdrop_reset(game);
    if (game_is_authority(game)) zombies_switch_mystery_box(game, -1, 0); // finishInit: switchMysteryBox()
}

void zombies_tick(Game *game, const float delta) {
    if (!game || !game->ready || game->end_timer > 0.0f) return;
    const float delta_ms = delta * 1000.0f * (game->config.delta_mlt > 0.0f ? game->config.delta_mlt : 1.0f);
    powerups_tick(game, delta_ms);
    airdrop_tick(game, delta_ms);
    if (game_is_authority(game)) {
        boxes_tick(game, delta_ms);
        krums_tick(game, delta_ms);
    } else {
        machines_client_tick(game, delta_ms);
    }
}

void zombies_send_state(Game *game, const int uid) {
    GameZombies *z = &game->zombies;
    for (int i = 0; i < ZOMBIES_POWERUPS; i++) {
        if (z->powerups[i] > 0.0f) powerup_send(game, i, uid); // syncData.pwup
    }
    for (int i = 0; i < z->box_count; i++) machine_sync(game, &z->boxes[i], ZOMBIE_MACHINE_MYSTERY_BOX, uid); // syncData.mybx
    for (int i = 0; i < z->krum_count; i++) machine_sync(game, &z->krums[i], ZOMBIE_MACHINE_KRUM, uid);      // syncData.krm
    if (z->wave > 0) {
        NetZombieWavePacket wave;
        memset(&wave, 0, sizeof(wave));
        wave.wave = z->wave;
        game_send(game, uid, NET_PACKET_ZOMBIE_WAVE, &wave, sizeof(wave));
    }
    airdrop_send_all(game, uid);
}

// zom.js endStats zombies / rounds: every player's aiKills and rounds, sent before the end table
// (NET_PACKET_MODE_END's stats array is full, see NetZombieStatsPacket)
void zombies_send_end_stats(Game *game) {
    if (!game || !game_is_authority(game)) return;
    NetZombieStatsPacket packet;
    memset(&packet, 0, sizeof(packet));
    for (size_t i = 0; i < game->player_count && packet.count < NET_MODE_END_MAX; i++) {
        const Player *p = game->players[i];
        if (!p) continue;
        NetZombieStatsEntry *e = &packet.entries[packet.count++];
        e->uid = p->uid;
        e->zombies = (int16_t) (p->mode.ai_kills > 32767 ? 32767 : p->mode.ai_kills);
        e->rounds = (int16_t) (p->mode.rounds > 32767 ? 32767 : p->mode.rounds);
    }
    game_send(game, NET_TARGET_ALL, NET_PACKET_ZOMBIE_STATS, &packet, sizeof(packet));
}

// ------------------------------------------------------------------ client mirror

#define ZREAD(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

int zombies_apply_packet(Game *game, Player *me, const int type, const void *payload, const uint16_t length) {
    GameZombies *z = &game->zombies;
    switch (type) {
        case NET_PACKET_ZOMBIE_MACHINE: { // Game.updateMysteryBox / updateKrumMachine
            ZREAD(NetZombieMachinePacket, packet);
            int kind = -1;
            ZombieMachine *m = find_machine(game, packet.uid, &kind);
            Object *o = object_at(game, packet.uid);
            if (!m || !o) return 1;
            o->active = !!packet.active;
            m->state = packet.state;
            m->timer = packet.timer;
            m->pending_weapon = packet.pending_weapon;
            m->purchased_by = packet.purchased_by;
            return 1;
        }
        case NET_PACKET_MYSTERY_BOX_SWITCH: {
            ZREAD(NetMysteryBoxSwitchPacket, packet);
            if (!game_is_authority(game)) zombies_switch_mystery_box(game, packet.uid, packet.enable);
            return 1;
        }
        case NET_PACKET_POWERUP: {
            ZREAD(NetPowerupPacket, packet);
            if (packet.index < ZOMBIES_POWERUPS) z->powerups[packet.index] = (float) packet.ms;
            return 1;
        }
        case NET_PACKET_ZOMBIE_WAVE: {
            ZREAD(NetZombieWavePacket, packet);
            z->wave = packet.wave;
            memcpy(z->special, packet.special, sizeof(z->special));
            z->special[sizeof(z->special) - 1] = 0;
            return 1;
        }
        case NET_PACKET_RESERVE_AMMO: {
            ZREAD(NetReserveAmmoPacket, packet);
            if (!me || game_is_authority(game)) return 1;
            me->reserve_count = packet.count < PLAYER_RESERVE_MAX ? packet.count : PLAYER_RESERVE_MAX;
            for (int i = 0; i < me->reserve_count; i++) {
                me->reserve_weapons[i] = packet.weapons[i];
                me->reserve_ammo[i] = packet.ammo[i];
            }
            return 1;
        }
        case NET_PACKET_SHARING: {
            ZREAD(NetSharingPacket, packet);
            if (me) me->sharing_is_caring = !!packet.on;
            return 1;
        }
        case NET_PACKET_ZOMBIE_STATS: { // the end table's zombies / rounds columns
            ZREAD(NetZombieStatsPacket, packet);
            if (game_is_authority(game)) return 1;
            for (int i = 0; i < packet.count && i < NET_MODE_END_MAX; i++) {
                for (size_t k = 0; k < game->player_count; k++) {
                    Player *p = game->players[k];
                    if (!p || p->uid != packet.entries[i].uid) continue;
                    p->mode.ai_kills = packet.entries[i].zombies;
                    p->mode.rounds = packet.entries[i].rounds;
                }
            }
            return 1;
        }
        case NET_PACKET_AIRDROP:
        case NET_PACKET_AIRDROP_REMOVE:
        case NET_PACKET_AIRDROP_CLAIM:
            return airdrop_apply_packet(game, type, payload, length);
        default:
            return 0;
    }
}
