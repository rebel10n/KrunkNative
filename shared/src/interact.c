// Interaction (Krunker v7.1.4 players.interact / confirmInteract / dropWeapon, src/data/players.js
// :3278-3606, and Player's changeInteract prompts, src/data/player.js:1352-1408, 1696-1705;
// doc/v7/map-logic.md §2.3, §2.6, §3.8 "Interact flow"). The client sends "int" type, uid (its
// lastInteractData) / "intSec" type, uid, the server checks the target, its tRadius, teamCheck
// and config.interactTimer: gates (0), deposit boxes (1), weapon pickups (6) and onInteractKey
// triggers (7, with the KR-cost "cnfm" confirmation). The zombies machines (13-15) and prize
// wheels (9) are the zombies / store packages'. Terminals are gone in v7 (TERMINAL builds
// nothing). Gates / banks keep their state in shared/src/triggers.c; pickups keep theirs in the
// trigger runtime (TriggerRuntime.pickups, reset by trigger_round_reset).
#include <shared.h>
#include <modes.h>
#include "modes/mode_internal.h"
#include "triggers_internal.h"
#include <math.h>
#include <pcg_basic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int player_collides(Player*, const Object*, float pad); // shared/src/player.c (collidesE)
int game_object_shootable(const Object*);               // shared/src/player.c (the hitscan object filter)

#define PICKUP_ZONE_X 12.0f // config.pickupZoneX / Z / H: the pickup GameObject is x +- 12, z +- 4,
#define PICKUP_ZONE_Z 4.0f  // centre y + pickupZoneH / 2, half height pickupZoneH * 2
#define PICKUP_ZONE_H 2.0f
#define PICKUP_UNRESTRICTED_UPGRADED 4500 // mode.unrestrictedWeapons: the price of an upgraded weapon's ammo

static const Weapon *weapon_at(const Game *game, const int id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : g_weapons;
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

static size_t weapon_total(const Game *game) {
    return game->weapon_count ? game->weapon_count : WEAPON_COUNT;
}

// Player.collides(box, pad) for a GameObject centred at c with half extents h (pad on x / z only).
static int collides_box(const Player *p, const vec3 c, const vec3 h, const float pad) {
    return p->position.x - p->scale < c.x + h.x + pad && p->position.x + p->scale > c.x - h.x - pad &&
           p->position.z - p->scale < c.z + h.z + pad && p->position.z + p->scale > c.z - h.z - pad &&
           p->position.y <= c.y + h.y && p->position.y + p->height >= c.y - h.y;
}

// The GameObject of a KN object (base centre, full size): centre y + h / 2, half extents s / 2.
static int collides_padded(const Player *p, const Object *o, const float pad) {
    const vec3 c = {o->position.x, o->position.y + o->scale.y * 0.5f, o->position.z};
    const vec3 h = {o->scale.x * 0.5f, o->scale.y * 0.5f, o->scale.z * 0.5f};
    return collides_box(p, c, h, pad);
}

static int collides_pickup(const Player *p, const GamePickup *k) {
    const vec3 c = {k->position.x, k->position.y + PICKUP_ZONE_H * 0.5f, k->position.z};
    const vec3 h = {PICKUP_ZONE_X, PICKUP_ZONE_H * 2.0f, PICKUP_ZONE_Z};
    return collides_box(p, c, h, 0.0f);
}

// game.teamCheck(obj, player)
static int team_allows(const Game *game, const Player *p, const int team) {
    return !team || !game->mode || !game->mode->config.teams || team == p->team;
}

// Gate / pickup "md": 0 score (paid), 1 score (kept), 2 kills, 3 account, 4 verified, 5 premium,
// 6 partner, 7 level (players.interact; the account facts come from trigger_account).
static int method_allows(const Game *game, const Player *p, const int method, const int price, const int points) {
    AccountInfo account;
    const int has_account = trigger_account(game, p, &account);
    switch (method) {
        case 0:
        case 1: return p->score >= price;
        case 2: return (int) p->kills >= points;
        case 3: return has_account;
        case 4: return has_account && account.verified;
        case 5: return has_account && account.premium;
        case 6: return has_account && account.partner;
        case 7: return has_account && account.level >= points;
        default: return 0;
    }
}

static void send_purchase(Game *game, Player *player, const int method, const int price) {
    NetMedalPacket medal; // send("am", [method ? "Unlocked" : "Purchased", method ? null : -price, 0, 1])
    memset(&medal, 0, sizeof(medal));
    snprintf(medal.text, sizeof(medal.text), "%s", method ? "Unlocked" : "Purchased");
    medal.score = method ? 0 : -price;
    game_send(game, player->uid, NET_PACKET_MEDAL, &medal, sizeof(medal));
    if (!method) player_score(player, -price, 0);
}

static int mode_blocks_pickups(const Game *game, const Player *p) {
    const GameModeConfig *m = game->mode ? &game->mode->config : NULL;
    if (!m) return 0;
    if (m->no_pickups) return 1;
    const char *team = p->team >= 1 && p->team <= 2 ? m->team_options[p->team - 1] : NULL;
    return team && !strcmp(team, "Infected"); // teamOptions[team - 1] == "Infected"
}

static int site_mode(const Game *game) {
    return game->mode && game->mode->config.site;
}

// ---------------------------------------------------------------- pickups (game.updatePickup / resetPickups)

static GamePickup *pickup_of(Game *game, const int uid) {
    TriggerRuntime *rt = trigger_runtime(game);
    if (!rt || uid < 0 || (size_t) uid >= rt->object_count || !game->map->objects[uid]->pickup) return NULL;
    return &rt->pickups[uid];
}

static int json_int(const cJSON *obj, const char *key, int *present) {
    const cJSON *v = cJSON_GetObjectItem(obj, key);
    if (present) *present = v && !cJSON_IsNull(v);
    if (cJSON_IsNumber(v)) return (int) cJSON_GetNumberValue(v);
    const char *text = cJSON_GetStringValue(v);
    return text ? atoi(text) : 0;
}

// map.js WEAPON_PICKUP -> addWeapon: w || wid (12 -> 0), sp, nm, md, wr, rw, rwg, tm
static void pickup_parse(GamePickup *k, const Object *o) {
    memset(k, 0, sizeof(*k));
    const cJSON *raw = o->raw;
    int present;
    int weapon = json_int(raw, "w", &present);
    if (!weapon) weapon = json_int(raw, "wid", NULL);
    if (weapon == WEAPON_ID_KNIFE) weapon = 0;
    k->weapon = weapon;
    k->score = json_int(raw, "sp", NULL);
    k->method = json_int(raw, "md", NULL);
    k->team = json_int(raw, "tm", NULL);
    k->no_msg = (unsigned char) (cJSON_GetObjectItem(raw, "nm") && !cJSON_IsNull(cJSON_GetObjectItem(raw, "nm")));
    const cJSON *wr = cJSON_GetObjectItem(raw, "wr");
    k->replace = !(wr && !cJSON_IsNull(wr) && !cJSON_IsFalse(wr) && !(cJSON_IsNumber(wr) && !cJSON_GetNumberValue(wr))); // pickupRep: !noRep
    k->random = (unsigned char) (cJSON_GetObjectItem(raw, "rw") && !cJSON_IsNull(cJSON_GetObjectItem(raw, "rw")));
    int group = json_int(raw, "rwg", &present);
    k->random_group = present && group ? (group < 1 ? 1 : group > 10000 ? 10000 : group) : 0;
    k->origin = o->position;
    k->position = o->position;
}

static int random_weapon(const Game *game) { // UTILS.getRandom(weapons.onlyWeapons)
    int pool[128], n = 0;
    for (int i = 0; i < (int) weapon_total(game) && n < 128; i++) {
        const Weapon *w = weapon_at(game, i);
        if (w && !w->melee && !w->equipment && !w->custom_only) pool[n++] = i;
    }
    return n ? pool[pcg32_boundedrand((uint32_t) n)] : 0;
}

void interact_reset_pickups(Game *game) {
    TriggerRuntime *rt = trigger_runtime(game);
    if (!rt || !game->map) return;
    int groups[64][2]; // rwg -> weapon
    int group_count = 0;
    for (size_t i = 0; i < rt->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!o->pickup) continue;
        GamePickup *k = &rt->pickups[i];
        const GamePickup old = *k;
        pickup_parse(k, o);
        if (k->random) {
            if (game_is_authority(game)) { // game.isServer && pickupRand: orgPickup = a random weapon
                int weapon = random_weapon(game);
                for (int g = 0; k->random_group && g < group_count; g++) {
                    if (groups[g][0] == k->random_group) weapon = groups[g][1];
                }
                if (k->random_group && group_count < 64) {
                    int known = 0;
                    for (int g = 0; g < group_count; g++) known |= groups[g][0] == k->random_group;
                    if (!known) {
                        groups[group_count][0] = k->random_group;
                        groups[group_count++][1] = weapon;
                    }
                }
                k->weapon = weapon;
            } else if (old.random) {
                k->weapon = old.weapon; // a client keeps the server's last pick until it hears the new one
            }
        }
        const Weapon *w = weapon_at(game, k->weapon);
        k->ammo = w ? (int) w->ammo : 0;           // pickupAmmo = weaponConfig[pickup || 0].ammo
        k->active = !w || !w->bomb || site_mode(game); // bombs only in site modes
        rt->pickup_moved[i] = 0;
    }
}

static void pickup_send(Game *game, const int target, const int uid) {
    const GamePickup *k = pickup_of(game, uid);
    if (!k) return;
    const TriggerRuntime *rt = game->trigger_rt;
    const NetPickupPacket packet = {
        .uid = uid,
        .weapon = k->weapon,
        .active = k->active,
        .moved = rt->pickup_moved[uid],
        .position = {k->position.x, k->position.y, k->position.z},
        .ammo = k->ammo,
    };
    game_send(game, target, NET_PACKET_PICKUP, &packet, sizeof(packet));
}

// "pkups" [uid, pickup, orgPickup, active, [x, y, z]] for joiners: every pickup (random weapons too)
void interact_send_pickups(Game *game, const int uid) {
    const TriggerRuntime *rt = trigger_runtime(game);
    for (size_t i = 0; rt && i < rt->object_count; i++) {
        if (game->map->objects[i]->pickup) pickup_send(game, uid, (int) i);
    }
}

// game.updatePickup(uid, pickup, orgPickup, active, pos, pickupAmmo)
static void pickup_update(Game *game, const int uid, const int weapon, const int active, const vec3 *position, const int ammo) {
    GamePickup *k = pickup_of(game, uid);
    if (!k) return;
    const Weapon *w = weapon_at(game, weapon);
    k->ammo = ammo >= 0 ? ammo : w ? (int) w->ammo : 0;
    k->position = position ? *position : k->origin;
    game->trigger_rt->pickup_moved[uid] = position != NULL;
    k->weapon = weapon;
    k->active = (unsigned char) !!active;
}

int game_pickup_state(const Game *game, const int uid, GamePickup *out) {
    const TriggerRuntime *rt = game ? game->trigger_rt : NULL;
    if (!rt || rt->map != game->map || uid < 0 || (size_t) uid >= rt->object_count || !game->map->objects[uid]->pickup) return 0;
    if (out) *out = rt->pickups[uid];
    return 1;
}

void game_pickup_set(Game *game, const int uid, const int weapon, const int active, const vec3 *position, const int ammo) {
    pickup_update(game, uid, weapon, active, position, ammo);
}

// ---------------------------------------------------------------- prompts (changeInteract)

typedef struct {
    GameInteractScan *out;
    int priority;     // vO422.priority (-1 = null)
} Scan;

// changeInteract(player, obj, type, data, priority): lastInteractData follows every call whose type
// is not 3 / 10 / 11 / 12; the prompt takes a call with no priority and another type, or one whose
// priority is >= the current one; noMsg objects never prompt.
static void change_interact(Scan *s, const int uid, const int type, const int data, const int priority, const int no_msg) {
    if (type != 3 && type != 10 && type != 11 && type != 12) {
        s->out->last_uid = uid;
        s->out->last_type = type;
    }
    if (no_msg) return;
    const int current = s->priority < 0 ? 0 : s->priority;
    if ((priority < 0 && type != s->out->type) || (priority < 0 ? 0 : priority) >= current) {
        s->priority = priority;
        s->out->type = type;
        s->out->index = uid;
        s->out->data = data;
    }
}

void game_interact_scan(const Game *game, const Player *player, GameInteractScan *out) {
    out->type = out->last_type = GAME_INTERACT_NONE;
    out->index = out->last_uid = -1;
    out->data = 0;
    if (!game || !game->map || !player || !player->active || player->noclip) return;
    Scan s = {.out = out, .priority = -1};

    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        const int interact_key = trigger_has_event(o, TRIGGER_EVENT_ON_INTERACT_KEY);
        // tRadius objects: gates / banks (7), onInteractKey triggers (7); (machines: zombies package)
        if ((o->gate || o->bank || interact_key) && (o->active || o->closeable) &&
            collides_padded(player, o, GAME_INTERACT_RADIUS) && team_allows(game, player, (int) o->team)) {
            if (o->gate) change_interact(&s, (int) i, GAME_INTERACT_GATE, 0, -1, o->no_msg);
            else if (o->bank) change_interact(&s, (int) i, GAME_INTERACT_BANK, 0, -1, 0);
            else change_interact(&s, (int) i, GAME_INTERACT_KEY, 0, -1, 0);
        }
        // collidesE(obj, collPad): onHoldMouse, "Hold [M1]" with priority 1
        if (o->active && trigger_has_event(o, TRIGGER_EVENT_ON_HOLD_MOUSE) && player_collides((Player *) player, o, game_constants.collision_padding)) {
            change_interact(&s, (int) i, GAME_INTERACT_TRIGGER, 0, 1, 0);
        }
    }

    if (!mode_blocks_pickups(game, player)) { // (!mode || !mode.noPickups), not an Infected
        const TriggerRuntime *rt = game->trigger_rt;
        for (size_t i = 0; rt && rt->map == game->map && i < rt->object_count; i++) {
            if (!game->map->objects[i]->pickup) continue;
            const GamePickup *k = &rt->pickups[i];
            if (k->weapon < 0 || !collides_pickup(player, k)) continue;
            const Weapon *w = weapon_at(game, k->weapon);
            if (k->active && team_allows(game, player, k->team) && (!w || !w->bomb || site_mode(game))) {
                change_interact(&s, (int) i, GAME_INTERACT_PICKUP, 0, -1, k->no_msg);
            }
        }
    }
}

int game_interact_prompt(const Game *game, const Player *player, int *object_index) {
    GameInteractScan scan;
    game_interact_scan(game, player, &scan);
    if (object_index) *object_index = scan.index;
    return scan.type;
}

static float round2(const float v) {
    return roundf(v * 100.0f) / 100.0f; // JS v.round(2)
}

// Krunker v3.1.1 players.spray (src/data/players.js): the view ray (sprayRange long, from the
// eye) against map.manager.objects; on the nearest hit face the decal (sprayScale wide; no
// sprays.js / type-4 store entry of v3.1.1 sets sizeReq) is pushed back inside the face, a
// face too small for it is no spray. config.isComp (the competitive host) has no
// KrunkNative counterpart. KrunkNative boxes are base-centred (position = bottom centre,
// scale = full size): Krunker's G.y / G.height are the centre / half height.
void game_player_spray(Game *game, Player *player) {
    if (!game || !player || !player->active || !game->map || !game_is_authority(game)) return;
    if (game->now - player->last_spray < GAME_SPRAY_TIMER) return;

    const float yaw = player->direction.y, pitch = player->direction.x;
    const vec3 eye = {player->position.x, player->position.y + player->height - game_constants.camera_height, player->position.z};
    const vec3 dir = {
        GAME_SPRAY_RANGE * sinf(yaw + (float) M_PI) * cosf(pitch),
        GAME_SPRAY_RANGE * sinf(pitch),
        GAME_SPRAY_RANGE * cosf(yaw + (float) M_PI) * cosf(pitch),
    };
    const Object *hit = NULL;
    float best = 0.0f;
    for (size_t i = 0; i < game->map->object_count; i++) {
        const Object *o = game->map->objects[i];
        if (!game_object_shootable(o) || o->no_vis) continue; // !noShoot && !noVis && active
        const float t = line_in_rect(eye, dir, o->position, o->scale);
        if (t <= 0.0f || t > 1.0f || (hit && t >= best)) continue; // L.sort(orderByDst)[0]
        hit = o;
        best = t;
    }
    if (!hit) return;

    const float w = hit->scale.x * 0.5f, h = hit->scale.y * 0.5f, l = hit->scale.z * 0.5f;
    const float cx = hit->position.x, cy = hit->position.y + h, cz = hit->position.z;
    const float range = GAME_SPRAY_RANGE * best - 0.1f;
    float y = eye.y + range * sinf(pitch);
    float x = eye.x + range * sinf(yaw + (float) M_PI) * cosf(pitch);
    float z = eye.z + range * cosf(yaw + (float) M_PI) * cosf(pitch);
    int x_dr = 0, y_dr = 0; // a0 (rotation.y) / a1 (rotation.x)
    if (y >= cy + h) y_dr = -90;
    else if (y <= cy - h) y_dr = 90;
    else if (x <= cx - w) x_dr = -90;
    else if (x >= cx + w) x_dr = 90;
    else if (z <= cz - l) x_dr = 180;

    const float half = GAME_SPRAY_SCALE / 2.0f;
    if (y_dr) {
        if (w < half || l < half) return;
        if (z - half < cz - l) z = cz - l + half;
        else if (z + half > cz + l) z = cz + l - half;
        if (x - half < cx - w) x = cx - w + half;
        else if (x + half > cx + w) x = cx + w - half;
    } else {
        if (h < half) return;
        if (x_dr == 90 || x_dr == -90) {
            if (l < half) return;
            if (z - half < cz - l) z = cz - l + half;
            else if (z + half > cz + l) z = cz + l - half;
        } else {
            if (w < half) return;
            if (x - half < cx - w) x = cx - w + half;
            else if (x + half > cx + w) x = cx + w - half;
        }
        if (y + half > cy + h) y = cy + h - half;
    }

    player->last_spray = game->now;
    game_play_sound(game, "spray", player->uid, player->position, 0.4f, 0.0f); // playSound(networkSounds "spray", 0.4, sid, .., true)
    const NetSprayPacket packet = {
        .uid = player->uid,
        .spray_index = player->spray_index,
        .position = {round2(x), round2(y), round2(z)},
        .y_rot = (int16_t) x_dr,
        .x_rot = (int16_t) y_dr,
    };
    game_send(game, NET_TARGET_ALL, NET_PACKET_SPRAY, &packet, sizeof(packet)); // IO.broadcast("sp", ...)
}

// ---------------------------------------------------------------- players.interact

static void use_gate(Game *game, Player *player, Object *gate) {
    const int method = gate->gate_method;
    if (!method_allows(game, player, method, (int) gate->score_points, (int) gate->score_points)) return;
    if (gate->score_points) send_purchase(game, player, method, (int) gate->score_points);
    game_set_gate(game, gate, !gate->active); // broadcast("gte", uid, active)
}

// Deposit (interactKey) or withdraw (interactSecKey) depositAmnt / withdrawAmnt (0 = all).
static void use_bank(Game *game, Player *player, Object *bank, const int withdraw) {
    if (bank->deposited > 0 && withdraw) {
        if (bank->withdraw_amount && bank->deposited > bank->withdraw_amount) {
            bank->deposited -= bank->withdraw_amount;
            player_score(player, bank->withdraw_amount, 0);
        } else {
            player_score(player, bank->deposited, 0);
            bank->deposited = 0;
        }
    } else if (player->score != 0 && !withdraw) {
        if (bank->deposit_amount && player->score > bank->deposit_amount) {
            bank->deposited += bank->deposit_amount;
            player_score(player, -bank->deposit_amount, 0);
        } else {
            bank->deposited += player->score;
            player_score(player, -player->score, 0);
        }
    }
    trigger_send_object_state(game, trigger_map_uid(game, bank), NET_OBJECT_BANK, bank->deposited); // "bnk"
}

static int loadout_index_of(const Player *p, const int weapon) {
    for (int i = 0; p->loadout && i < p->loadout_size; i++) {
        if (p->loadout[i] == weapon) return i;
    }
    return -1;
}

// addPicked / removePicked: lastPicked[weapon] = the pickup's uid (the first one only)
static void add_picked(Player *p, const int weapon, const int uid) {
    if (weapon >= 0 && weapon < PLAYER_PICKED_MAX && !p->last_picked[weapon]) p->last_picked[weapon] = uid + 1;
}

static int remove_picked(Player *p, const int weapon) {
    if (weapon < 0 || weapon >= PLAYER_PICKED_MAX || !p->last_picked[weapon]) return 0;
    p->last_picked[weapon] = 0;
    return 1;
}

// The loadout with ids[] (its ammo per slot) in hand at `index`, sorted by weapon type (keeping the
// ammo aligned), then "inat" to everyone.
static void set_loadout(Game *game, Player *p, int *ids, unsigned int *ammo, const int count, const int sort, const int hold) {
    if (sort) { // loadout.sort(by weaponConfig[id].type), stable
        for (int i = 1; i < count; i++) {
            for (int k = i; k > 0; k--) {
                const Weapon *a = weapon_at(game, ids[k - 1]), *b = weapon_at(game, ids[k]);
                if (!a || !b || a->type <= b->type) break;
                const int t = ids[k];
                ids[k] = ids[k - 1];
                ids[k - 1] = t;
                const unsigned int m = ammo[k];
                ammo[k] = ammo[k - 1];
                ammo[k - 1] = m;
            }
        }
    }
    game_player_changing(game, p, 0);
    mode_set_player_loadout(p, ids, count, 0);
    for (int i = 0; i < p->loadout_size && i < count; i++) {
        if (p->loadout[i] == ids[i]) p->ammo[i] = ammo[i];
    }
    const int index = loadout_index_of(p, hold);
    if (index >= 0 && index != p->loadout_index) player_swap_weapon(p, index, 1, 0, 0);
    game_player_changing(game, p, 1);
    mode_send_loadout(game, p); // broadcast("inat", sid, loadout, loadoutIndex, ...)
}

// interact type 6: one targeted weapon pickup (players.js:3283-3367)
static void use_pickup(Game *game, Player *p, const int uid) {
    GamePickup *k = pickup_of(game, uid);
    if (!k || mode_blocks_pickups(game, p) || !k->active || k->weapon < 0 || !collides_pickup(p, k) || !p->loadout) return;
    const Weapon *w = weapon_at(game, k->weapon);
    const int unrestricted = game->mode && game->mode->config.unrestricted_weapons; // mode.unrestrictedWeapons (Zombies)
    const int held = loadout_index_of(p, k->weapon);
    if (!w || (held >= 0 && !unrestricted) || !team_allows(game, p, k->team) || (w->bomb && !site_mode(game))) return;
    // unrestricted: a held weapon's ammo costs half (4500 when upgraded); buying never empties the pickup
    const int upgraded = held >= 0 && held < 8 && (p->upgraded_slots >> held & 1);
    const int price = unrestricted && held >= 0 && k->method < 2 ? (upgraded ? PICKUP_UNRESTRICTED_UPGRADED : (k->score + 1) / 2) : k->score;
    if (!method_allows(game, p, k->method, price, k->score)) return;
    if (unrestricted) { // giveWeapon(player, pickup, false, true): the weapon, full ammo
        player_give_weapon(p, k->weapon);
        const int slot = loadout_index_of(p, k->weapon);
        if (slot >= 0 && p->ammo) {
            const Weapon *have = player_slot_weapon(p, slot);
            p->ammo[slot] = have ? have->ammo : w->ammo;
            mode_send_loadout(game, p);
        }
        if (price && !k->method) send_purchase(game, p, 0, price);
        return;
    }

    int ids[NET_LOADOUT_MAX + 1];
    unsigned int ammo[NET_LOADOUT_MAX + 1];
    int count = 0;
    for (int i = 0; i < p->loadout_size && count < NET_LOADOUT_MAX; i++) {
        ids[count] = p->loadout[i];
        ammo[count++] = p->ammo ? p->ammo[i] : 0;
    }
    const int picked = k->weapon;
    const int picked_ammo = k->ammo;
    int replaced = 0, old = -1;
    for (int i = 0; i < count; i++) {
        const Weapon *have = weapon_at(game, ids[i]);
        if (!have || have->type != w->type) continue;
        old = ids[i];
        // v7 removes lastPicked of loadout[0] here, not of the weapon it swaps out (v7 bug, fixed:
        // the swapped-out weapon's entry goes)
        const int was_picked = remove_picked(p, old);
        if (k->replace && was_picked) add_picked(p, picked, uid);
        ids[i] = picked;
        if (k->replace) { // the pickup now holds the old weapon with its ammo
            k->weapon = old;
            k->ammo = (int) ammo[i];
        }
        ammo[i] = (unsigned int) picked_ammo;
        replaced = 1;
        break;
    }
    if (!replaced) {
        if (count >= NET_LOADOUT_MAX) return;
        if (k->replace) add_picked(p, picked, uid);
        ids[count] = picked;
        ammo[count++] = (unsigned int) picked_ammo;
        if (k->replace) k->weapon = -1; // pickup = undefined: emptied
    }
    set_loadout(game, p, ids, ammo, count, !replaced, picked);
    pickup_send(game, NET_TARGET_ALL, uid); // the pickup half of "inat" (pickupLeft, active)
    if (k->score && !k->method) send_purchase(game, p, 0, k->score);
}

// interact type 7: an onInteractKey trigger in range 7; a KR cost asks for confirmation ("cnfm")
static void use_interact_key(Game *game, Player *p, Object *o, const int uid) {
    if (!o || !trigger_has_event(o, TRIGGER_EVENT_ON_INTERACT_KEY) || !o->active || !collides_padded(p, o, TRIGGER_INTERACT_KEY_RADIUS) ||
        !team_allows(game, p, (int) o->team)) return;
    if (o->trig->action_cost && !o->trig->action_method) {
        AccountInfo account;
        if (trigger_account(game, p, &account) && account.kr >= o->trig->action_cost) {
            p->pending_trigger = uid + 1;
            p->pending_spin = 0;
            const NetConfirmPacket packet = {.uid = uid, .kind = 0};
            game_send(game, p->uid, NET_PACKET_CONFIRM_INTERACT, &packet, sizeof(packet)); // send("cnfm", uid)
        }
        return;
    }
    game_on_trigger(game, p, o, NULL);
}

void game_player_interact_v7(Game *game, Player *player, const int type, const int uid, const int secondary) {
    if (!game || !player || !player->active || !game->map || !game_is_authority(game)) return;
    if (game->end_timer > 0.0f) return; // the end screen (players.clear)
    player_drain_inputs(player); // io_int drains player.inputs first (the frames sent before the "int")
    if (!player->active) return;
    if (game->now - player->last_interact < GAME_INTERACT_TIMER) return; // config.interactTimer
    player->last_interact = game->now;
    Object *o = uid >= 0 && (size_t) uid < game->map->object_count ? game->map->objects[uid] : NULL;
    if (!secondary) {
        switch (type) {
            case GAME_INTERACT_PICKUP:
                use_pickup(game, player, uid);
                break;
            case GAME_INTERACT_GATE: // while no waitTimer blocks
                if (o && o->gate && (o->active || o->closeable) && !trigger_wait_blocks(game) &&
                    collides_padded(player, o, GAME_INTERACT_RADIUS) && team_allows(game, player, (int) o->team)) use_gate(game, player, o);
                break;
            case GAME_INTERACT_KEY:
                if (!trigger_wait_blocks(game)) use_interact_key(game, player, o, uid);
                break;
            default: // 9 prize wheels (KR store), 13-15 zombies machines: their packages
                break;
        }
    }
    if (type == GAME_INTERACT_BANK && o && o->bank && o->active && collides_padded(player, o, GAME_INTERACT_RADIUS) &&
        team_allows(game, player, (int) o->team)) use_bank(game, player, o, secondary);
}

// "cnfm": confirmInteract (kind 0: the pending onInteractKey trigger; 2: prize wheels, store)
void game_player_confirm_interact(Game *game, Player *player, const int kind) {
    if (!game || !player || !player->active || !game->map || !game_is_authority(game) || kind != 0) return;
    const int uid = player->pending_trigger - 1;
    if (uid < 0 || (size_t) uid >= game->map->object_count) return;
    Object *o = game->map->objects[uid];
    if (o && trigger_has_event(o, TRIGGER_EVENT_ON_INTERACT_KEY) && o->active && collides_padded(player, o, TRIGGER_INTERACT_KEY_RADIUS) &&
        team_allows(game, player, (int) o->team)) game_on_trigger(game, player, o, NULL);
}

// players.findEmptyPickup(uid): a pickupRep pickup that holds nothing now
static GamePickup *empty_pickup(Game *game, const int uid) {
    GamePickup *k = pickup_of(game, uid);
    return k && k->replace && k->weapon < 0 ? k : NULL;
}

static vec3 drop_position(const Player *p, const int spread) {
    return (vec3) {p->position.x + (float) trigger_rand_int(-spread, spread), p->position.y + 1.0f,
                   p->position.z + (float) trigger_rand_int(-spread, spread)};
}

// players.dropWeapon(player) ("drpw"): a weapon taken from a now-empty pickupRep pickup goes back
// into it, dropped next to the player; the rest of the loadout re-sorts, slot 0 in hand.
void game_player_drop_weapon(Game *game, Player *p) {
    if (!game || !p || !p->active || !game_is_authority(game) || !p->loadout || (game->mode && game->mode->config.no_pickups)) return;
    if (p->loadout_index < 0 || p->loadout_index >= p->loadout_size) return;
    const int weapon = p->loadout[p->loadout_index];
    const Weapon *w = weapon_at(game, weapon);
    if (!w || w->melee || (site_mode(game) && !w->bomb)) return;
    if (weapon >= PLAYER_PICKED_MAX || !p->last_picked[weapon]) return;
    const int pickup_uid = p->last_picked[weapon] - 1;
    GamePickup *k = empty_pickup(game, pickup_uid);
    const vec3 at = drop_position(p, 1);
    if (k) {
        remove_picked(p, weapon);
        pickup_update(game, pickup_uid, weapon, 1, &at, (int) p->ammo[p->loadout_index]);
    }
    int ids[NET_LOADOUT_MAX + 1];
    unsigned int ammo[NET_LOADOUT_MAX + 1];
    int count = 0;
    for (int i = 0; i < p->loadout_size && count < NET_LOADOUT_MAX; i++) {
        if (i == p->loadout_index) continue;
        ids[count] = p->loadout[i];
        ammo[count++] = p->ammo ? p->ammo[i] : 0;
    }
    if (!count) return;
    set_loadout(game, p, ids, ammo, count, 1, -1);
    if (p->loadout_index != 0) player_swap_weapon(p, 0, 1, 0, 0); // loadoutIndex = 0
    if (k) pickup_send(game, NET_TARGET_ALL, pickup_uid);
}

// dropWeapon(player, true) at a death / leave: every weapon from a now-empty pickupRep pickup goes
// back into it around the player (+-5); lastPicked is cleared.
void game_player_drop_all(Game *game, Player *p) {
    if (!game || !p || !game_is_authority(game) || !game->map || (game->mode && game->mode->config.no_pickups) || site_mode(game)) return;
    for (int i = 0; p->loadout && i < p->loadout_size; i++) {
        const int weapon = p->loadout[i];
        if (weapon < 0 || weapon >= PLAYER_PICKED_MAX || !p->last_picked[weapon]) continue;
        const Weapon *w = weapon_at(game, weapon);
        const int pickup_uid = p->last_picked[weapon] - 1;
        if (!w || w->melee || !empty_pickup(game, pickup_uid)) continue;
        const vec3 at = drop_position(p, 5);
        pickup_update(game, pickup_uid, weapon, 1, &at, (int) p->ammo[i]);
        pickup_send(game, NET_TARGET_ALL, pickup_uid); // broadcast("inat", -1, null, null, uid, weapon, true, true, pos)
    }
    memset(p->last_picked, 0, sizeof(p->last_picked));
}

// v3-shaped "int" (old clients, tests, bots): 0 spray, 1 interact with the target the server's own
// prompt scan finds, 2 drop.
void game_player_interact(Game *game, Player *player, const int action, const int secondary) {
    if (!game || !player || !player->active || !game->map || !game_is_authority(game)) return;
    // The end screen: the server's players are inactive already (players.clear); the offline
    // game keeps its players for the endAnim (end_keep_players), so no spray/door/pickup either.
    if (game->end_timer > 0.0f) return;
    // gameserver.ts io_int drains player.inputs first: the spray ray (and the interact /
    // drop position) must use the look and position of the frames sent before the "int".
    player_drain_inputs(player);
    if (!player->active) return;
    if (action == 0) { // io_int 0: players.spray (no interactTimer)
        game_player_spray(game, player);
        return;
    }
    if (action == 2) {
        game_player_drop_weapon(game, player);
        return;
    }
    if (action != 1) return;
    GameInteractScan scan;
    game_interact_scan(game, player, &scan);
    game_player_interact_v7(game, player, scan.last_type, scan.last_uid, secondary);
}
