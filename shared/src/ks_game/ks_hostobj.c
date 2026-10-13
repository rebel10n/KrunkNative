// KrunkScript host objects (doc/v7/krunkscript.md 3.2, createKrunkScriptApiContext.js vO444):
// player, AI, map object and path node wrappers, plus the shared GAME.* entries that return
// them (PLAYERS.list / findByID, OBJECTS.*, AI.* on the server). A wrapper's native is a
// KsRef (stable for the life of the script); the engine object is looked up by id on every
// access, so a wrapper of a player that left reads as undefined instead of dangling.
#include "ks_game_internal.h"

#include <ai.h>
#include <modes.h>
#include <weapons.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARG(i) ((i) < argc ? args[(i)] : ks_undef())

static KsRef *ref_of(void *native) { return native; }

Game *ks_native_game(void *native)
{
    KsRef *r = ref_of(native);
    return r && r->kg ? r->kg->game : NULL;
}

Player *ks_game_player_by_uid(Game *game, const uint32_t uid)
{
    return game ? game_find_player(game, (int) uid) : NULL;
}

Player *ks_player_of(void *native)
{
    KsRef *r = ref_of(native);
    if (!r || r->kind != KS_REF_PLAYER) return NULL;
    return ks_game_player_by_uid(ks_native_game(native), (uint32_t) r->id);
}

Player *ks_game_find_player_str(Game *game, const char *id)
{
    if (!game || !id) return NULL;
    char buf[24];
    for (size_t i = 0; i < game->player_count; i++) {
        Player *p = game->players[i];
        if (!p) continue;
        snprintf(buf, sizeof(buf), "%d", p->uid);
        if (!strcmp(buf, id)) return p;
    }
    return NULL;
}

Player *ks_game_player_by_id(Game *game, const KsValue id)
{
    KsScript *s = ks_game_script(game);
    if (!s) return NULL;
    char buf[64];
    if (id.tag == KS_UNDEF || id.tag == KS_NULL) return NULL; // undefined == "5" is false
    ks_to_string(s, id, buf, sizeof(buf));
    return ks_game_find_player_str(game, buf);
}

// isServer && io: the authority's server script (server-only writes and methods)
static int on_server(const KsRef *r)
{
    return r && r->kg && r->kg->side == KS_SIDE_SERVER && game_is_authority(r->kg->game);
}

static int is_number(const KsValue v) { return v.tag == KS_NUM && isfinite(v.as.num); }

// parseInt(ToString(v)) || 0
static int parse_int0(KsScript *s, const KsValue v)
{
    if (v.tag == KS_NUM) {
        const double d = v.as.num;
        if (!isfinite(d)) return 0;
        return (int) (d < 0 ? ceil(d) : floor(d));
    }
    char buf[64];
    ks_to_string(s, v, buf, sizeof(buf));
    const double d = strtod(buf, NULL); // digits prefix (no hex: KS ids / indices)
    if (!isfinite(d)) return 0;
    return (int) (d < 0 ? ceil(d) : floor(d));
}

static KsValue str_value(KsScript *s, const char *text) { return ks_str(s, text ? text : "", text ? strlen(text) : 0); }

// ---------------------------------------------------------------------------
// Player (3.2.1, API:53-553)

#define PLAYER_OR(r, fallback)               \
    KsRef *r = ref_of(native);               \
    Player *p = ks_player_of(native);        \
    (void) id;                               \
    if (!p) return fallback;

#define PLAYER_SET(r)                        \
    KsRef *r = ref_of(native);               \
    Player *p = ks_player_of(native);        \
    (void) id;                               \
    (void) r;                                \
    if (!p) return;

#define PLAYER_METHOD(r)                     \
    KsRef *r = ref_of(native);               \
    Player *p = ks_player_of(native);        \
    if (!p) return ks_undef();

static KsValue pl_id(KsScript *s, void *native, uint32_t id)
{
    PLAYER_OR(r, ks_undef());
    (void) r;
    char buf[24];
    ks_player_id(p, buf, sizeof(buf));
    return str_value(s, buf);
}
static KsValue pl_sid(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_num(p->uid); }
static KsValue pl_username(KsScript *s, void *native, uint32_t id) { PLAYER_OR(r, ks_undef()); (void) r; return str_value(s, p->name); }
static KsValue pl_is_you(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_bool(p->is_you); }
static KsValue pl_active(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_bool(p->active); }

static KsValue pl_account(KsScript *s, void *native, const uint32_t id, const int want_name)
{
    PLAYER_OR(r, ks_undef());
    char acct_id[64] = "", acct_name[64] = "";
    if (r->kg->side == KS_SIDE_SERVER && r->kg->account) r->kg->account(r->kg->host_user, p, acct_id, sizeof(acct_id), acct_name, sizeof(acct_name));
    return str_value(s, want_name ? acct_name : acct_id);
}
static KsValue pl_account_id(KsScript *s, void *native, uint32_t id) { return pl_account(s, native, id, 0); }
static KsValue pl_account_name(KsScript *s, void *native, uint32_t id) { return pl_account(s, native, id, 1); }

static KsValue pl_flag_get(void *native, const uint32_t id, const int bit)
{
    PLAYER_OR(r, ks_undef());
    (void) p;
    return ks_bool((r->allow & bit) != 0);
}
// !!v (ToBoolean): KS numbers / bools through ToNumber, everything else by type
static int truthy(const KsValue v)
{
    switch (v.tag) {
        case KS_UNDEF:
        case KS_NULL: return 0;
        case KS_BOOL: return v.as.b != 0;
        case KS_NUM: return v.as.num != 0.0 && !isnan(v.as.num);
        case KS_STR: {
            size_t n = 0;
            ks_str_utf8(v, &n);
            return n != 0;
        }
        default: return 1;
    }
}
#define FLAG_PROP(name, bit, invert)                                                                                   \
    static KsValue name##_get(KsScript *s, void *native, uint32_t id)                                                  \
    {                                                                                                                  \
        (void) s;                                                                                                      \
        KsValue v = pl_flag_get(native, id, bit);                                                                      \
        if ((invert) && v.tag == KS_BOOL) v.as.b = !v.as.b;                                                            \
        return v;                                                                                                      \
    }                                                                                                                  \
    static void name##_set(KsScript *s, void *native, uint32_t id, KsValue v)                                          \
    {                                                                                                                  \
        (void) s;                                                                                                      \
        PLAYER_SET(r);                                                                                                 \
        (void) p;                                                                                                      \
        const int on = truthy(v) != (invert);                                                                          \
        if (on) r->allow |= (bit);                                                                                     \
        else r->allow &= ~(bit);                                                                                       \
    }
FLAG_PROP(pl_default_movement, KS_PLAYER_DEFAULT_MOVEMENT, 0)
FLAG_PROP(pl_default_velocity, KS_PLAYER_DEFAULT_VELOCITY, 0)
FLAG_PROP(pl_default_rotation, KS_PLAYER_DEFAULT_ROTATION, 0)
FLAG_PROP(pl_disable_shooting, KS_PLAYER_CAN_SHOOT, 1)
FLAG_PROP(pl_disable_melee, KS_PLAYER_CAN_MELEE, 1)
#undef FLAG_PROP

static KsValue pl_disable_default(KsScript *s, void *native, const KsValue *args, int argc)
{
    PLAYER_METHOD(r);
    (void) p;
    static const char *const names[] = {"mouseY", "mouseX", "movDir", "lMouse", "rMouse", "jump",
                                        "reload", "crouch", "scroll", "swap",   "restK",  "inter"};
    const KsValue v = ARG(0);
    size_t n = 0;
    const char *name = ks_str_utf8(v, &n);
    (void) s;
    if (!name) return ks_undef(); // p17057 == "jump": a non-string never equals (loosely) these names
    for (int i = 0; i < (int) (sizeof(names) / sizeof(names[0])); i++)
        if (strlen(names[i]) == n && !memcmp(names[i], name, n)) r->disabled |= (uint16_t) (1u << i);
    return ks_undef();
}

static KsValue pl_asset_id(KsScript *s, void *native, uint32_t id)
{
    // config.aID: KN players have no custom model asset (updateAssetID needs config.mdl)
    PLAYER_OR(r, ks_undef());
    (void) r;
    (void) p;
    return str_value(s, "");
}
static void pl_asset_id_set(KsScript *s, void *native, uint32_t id, KsValue v) { (void) s; (void) native; (void) id; (void) v; }

static KsValue pl_on_wall(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_bool(p->on_wall != 0); }
static void pl_on_wall_set(KsScript *s, void *native, uint32_t id, KsValue v) { (void) s; PLAYER_SET(r); p->on_wall = truthy(v) ? (p->on_wall ? p->on_wall : 1) : 0; }
static KsValue pl_on_ground(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_bool(p->on_ground || p->on_ramp); }
static void pl_on_ground_set(KsScript *s, void *native, uint32_t id, KsValue v) { (void) s; PLAYER_SET(r); p->on_ground = truthy(v); }
static KsValue pl_on_terrain(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_bool(p->on_terrain); }
static void pl_on_terrain_set(KsScript *s, void *native, uint32_t id, KsValue v) { (void) s; PLAYER_SET(r); p->on_terrain = truthy(v); }
static KsValue pl_crouching(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_bool(p->crouch_val >= 1.0f); }
static void pl_crouching_set(KsScript *s, void *native, uint32_t id, KsValue v)
{
    (void) s;
    PLAYER_SET(r);
    p->crouch_val = truthy(v) ? 1.0f : 0.0f; // crouchVal = ~~!!v
    player_update_height(p);
}
static KsValue pl_team(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_num(p->team); }

static int weapon_count_of(const Game *g) { return g->weapon_count ? (int) g->weapon_count : WEAPON_COUNT; }
static const Weapon *weapon_at(const Game *g, const int i)
{
    if (i < 0 || i >= weapon_count_of(g)) return NULL;
    return g->weapons ? g->weapons[i] : g_weapons[i];
}
static int is_primary(const Weapon *w) { return w && !w->secondary && !w->melee && !w->equipment; }

// "inat" (NET_PACKET_LOADOUT) of the player's current loadout
static void send_loadout(Game *game, const Player *p)
{
    NetLoadoutPacket packet;
    memset(&packet, 0, sizeof(packet));
    packet.uid = p->uid;
    packet.count = player_loadout_base(p) < NET_LOADOUT_MAX ? player_loadout_base(p) : NET_LOADOUT_MAX; // the base: extra weapons ride in LOADOUT_EXTRA
    for (int i = 0; i < packet.count; i++) {
        packet.weapons[i] = p->loadout[i];
        packet.ammo[i] = p->ammo ? (int32_t) p->ammo[i] : 0;
    }
    packet.loadout_index = p->loadout_index;
    packet.weapon_tier = p->mode.weapon_tier;
    game_send(game, NET_TARGET_ALL, NET_PACKET_LOADOUT, &packet, sizeof(packet));
}

// players.updateLoadout(game, index, true, loadout): exactly these weapons (no knife added),
// fresh ammo, the weapon at `index` equipped; then "inat" to everyone.
static void set_loadout(Game *game, Player *p, const int *ids, const int count, int index)
{
    if (count <= 0 || count > NET_LOADOUT_MAX) return;
    int *loadout = calloc((size_t) count, sizeof(int));
    unsigned int *ammo = calloc((size_t) count, sizeof(unsigned int));
    float *reloads = calloc((size_t) count, sizeof(float));
    if (!loadout || !ammo || !reloads) {
        free(loadout);
        free(ammo);
        free(reloads);
        return;
    }
    const int ammo_limit = game->mode ? game->mode->config.ammo_limit : 0;
    for (int i = 0; i < count; i++) {
        const Weapon *w = weapon_at(game, ids[i]);
        loadout[i] = ids[i];
        ammo[i] = w ? (ammo_limit > 0 && w->ammo ? (unsigned int) ammo_limit : w->ammo) : 0;
    }
    game_player_changing(game, p, 0);
    free(p->loadout);
    free(p->ammo);
    free(p->reloads);
    p->loadout = loadout;
    p->ammo = ammo;
    p->reloads = reloads;
    p->loadout_size = count;
    p->loadout_base = count;
    player_loadout_append_extras(p); // KrunkNative Object Manager: the extra weapons stay after it
    p->can_throw = 1;
    if (index < 0 || index >= count) index = 0;
    p->loadout_index = -1; // force the swap to re-equip
    player_swap_weapon(p, index, 1, 0, 0);
    game_player_changing(game, p, 1);
    send_loadout(game, p);
}

static KsValue pl_class_index(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_num(p->class_index); }
static void pl_class_index_set(KsScript *s, void *native, uint32_t id, KsValue v)
{
    PLAYER_SET(r);
    if (!on_server(r)) return;
    Game *game = p->game;
    const int index = parse_int0(s, v);
    const int class_count = game->class_count ? (int) game->class_count : (int) (sizeof(g_classes) / sizeof(g_classes[0]));
    if (index < 0 || index >= class_count || index == p->class_index) return;
    p->class_index = index;
    // updateItems: the class's loadout (primary, its secondary, knife) and stats
    const ClassConfig *cc = game->classes ? &game->classes[index] : &g_classes[index];
    int ids[3], n = 0;
    const int primary = cc->loadout ? cc->loadout[0] : 1;
    if (weapon_at(game, primary)) ids[n++] = primary;
    const int sec = p->secondary_index >= 0 && weapon_at(game, p->secondary_index) && weapon_at(game, p->secondary_index)->secondary
                        ? p->secondary_index : WEAPON_ID_PISTOL;
    if (cc->secondary && !game->config.no_secondaries && weapon_at(game, sec) && sec != primary) ids[n++] = sec;
    if (weapon_at(game, WEAPON_ID_KNIFE) && primary != WEAPON_ID_KNIFE) ids[n++] = WEAPON_ID_KNIFE;
    p->speed = cc->speed ? cc->speed : 1.0f;
    p->wall_jump = cc->wall_jump;
    if (n) set_loadout(game, p, ids, n, 0);
}

static KsValue pl_loadout_index(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_num(p->loadout_index > 0 ? p->loadout_index : 0); }
static void pl_loadout_index_set(KsScript *s, void *native, uint32_t id, KsValue v)
{
    PLAYER_SET(r);
    if (!on_server(r)) return;
    const int index = parse_int0(s, v);
    if (index < 0 || index >= p->loadout_size) return;
    player_swap_weapon(p, index, 1, 0, 0); // swapWeapon(player, 0, true, undefined, index)
    send_loadout(p->game, p);
}

static KsValue pl_ammo(KsScript *s, void *native, uint32_t id)
{
    (void) s;
    PLAYER_OR(r, ks_undef());
    (void) r;
    if (!p->ammo || p->loadout_index < 0 || p->loadout_index >= p->loadout_size) return ks_num(0);
    return ks_num(p->ammo[p->loadout_index]);
}

// changePrimary / changeSecondary / giveWeapon / remove* / clearLoadout (API:210-397)
static KsValue pl_change_slot(KsScript *s, void *native, const KsValue *args, const int argc, const int secondary)
{
    PLAYER_METHOD(r);
    if (!on_server(r)) return ks_undef();
    Game *game = p->game;
    const int wid = parse_int0(s, ARG(0));
    const Weapon *w = weapon_at(game, wid);
    if (!w) return ks_undef();
    if (secondary ? !(w->secondary && !w->equipment && !w->melee) : !is_primary(w)) return ks_undef();
    int ids[NET_LOADOUT_MAX + 1], n = 0, replaced = 0;
    for (int i = 0; i < player_loadout_base(p) && n < NET_LOADOUT_MAX; i++) ids[n++] = p->loadout[i];
    for (int i = 0; i < n; i++) {
        const Weapon *cur = weapon_at(game, ids[i]);
        if (secondary ? (cur && cur->secondary) : is_primary(cur)) {
            if (ids[i] == wid) return ks_undef();
            ids[i] = wid;
            replaced = 1;
            break;
        }
    }
    if (!replaced) {
        if (n >= NET_LOADOUT_MAX) return ks_undef();
        const int at = secondary ? (n > 1 ? 1 : 0) : 0;
        memmove(&ids[at + 1], &ids[at], (size_t) (n - at) * sizeof(int));
        ids[at] = wid;
        n++;
    }
    set_loadout(game, p, ids, n, p->loadout_index);
    return ks_undef();
}
static KsValue pl_change_primary(KsScript *s, void *native, const KsValue *args, int argc) { return pl_change_slot(s, native, args, argc, 0); }
static KsValue pl_change_secondary(KsScript *s, void *native, const KsValue *args, int argc) { return pl_change_slot(s, native, args, argc, 1); }

static KsValue pl_give_weapon(KsScript *s, void *native, const KsValue *args, int argc)
{
    PLAYER_METHOD(r);
    if (!on_server(r)) return ks_undef();
    Game *game = p->game;
    const int wid = parse_int0(s, ARG(0));
    const Weapon *w = weapon_at(game, wid);
    if (!w || w->equipment) return ks_undef();
    int ids[NET_LOADOUT_MAX + 1], n = 0, placed = -1, primary_at = -1;
    for (int i = 0; i < player_loadout_base(p) && n < NET_LOADOUT_MAX; i++) ids[n++] = p->loadout[i];
    for (int i = 0; i < n; i++) {
        const Weapon *cur = weapon_at(game, ids[i]);
        if (ids[i] == wid) return ks_undef();
        if (is_primary(cur)) primary_at = i;
        if ((is_primary(w) && is_primary(cur)) || (w->secondary && cur && cur->secondary) || (w->melee && cur && cur->melee)) {
            ids[i] = wid;
            placed = i;
            break;
        }
    }
    int index = p->loadout_index;
    if (placed < 0) {
        if (n >= NET_LOADOUT_MAX) return ks_undef();
        index = 0; // loadoutIndex = undefined -> the first slot
        if (w->melee) {
            ids[n++] = wid;
        } else {
            const int at = w->secondary ? (primary_at == -1 ? 0 : 1) : 0;
            memmove(&ids[at + 1], &ids[at], (size_t) (n - at) * sizeof(int));
            ids[at] = wid;
            n++;
        }
    }
    set_loadout(game, p, ids, n, index);
    return ks_undef();
}

static KsValue pl_remove_kind(KsScript *s, void *native, const KsValue *args, int argc, const int kind)
{
    (void) s;
    (void) args;
    (void) argc;
    PLAYER_METHOD(r);
    if (!on_server(r)) return ks_undef();
    Game *game = p->game;
    int at = -1;
    for (int i = 0; i < player_loadout_base(p); i++) {
        const Weapon *cur = weapon_at(game, p->loadout[i]);
        if ((kind == 0 && is_primary(cur)) || (kind == 1 && cur && cur->secondary) || (kind == 2 && cur && cur->melee)) {
            at = i;
            break;
        }
    }
    if (at < 0 || (kind == 2 && p->loadout_size <= 1) || p->loadout_size <= 1) return ks_undef();
    int ids[NET_LOADOUT_MAX], n = 0;
    for (int i = 0; i < player_loadout_base(p) && n < NET_LOADOUT_MAX; i++)
        if (i != at) ids[n++] = p->loadout[i];
    int index = p->loadout_index;
    if (at < index) index--;
    set_loadout(game, p, ids, n, index);
    return ks_undef();
}
static KsValue pl_remove_primary(KsScript *s, void *native, const KsValue *args, int argc) { return pl_remove_kind(s, native, args, argc, 0); }
static KsValue pl_remove_secondary(KsScript *s, void *native, const KsValue *args, int argc) { return pl_remove_kind(s, native, args, argc, 1); }
static KsValue pl_remove_melee(KsScript *s, void *native, const KsValue *args, int argc) { return pl_remove_kind(s, native, args, argc, 2); }

static KsValue pl_clear_loadout(KsScript *s, void *native, const KsValue *args, int argc)
{
    (void) s;
    (void) args;
    (void) argc;
    PLAYER_METHOD(r);
    if (!on_server(r)) return ks_undef();
    if (p->loadout_size == 1 && p->loadout[0] == WEAPON_ID_KNIFE) return ks_undef();
    const int knife = WEAPON_ID_KNIFE;
    set_loadout(p->game, p, &knife, 1, 0);
    return ks_undef();
}

static KsValue pl_score(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_num(p->score); }
static void pl_score_set(KsScript *s, void *native, uint32_t id, KsValue v)
{
    PLAYER_SET(r);
    if (!on_server(r) || !is_number(v)) return;
    // players.score(player, parseInt(v) || 0, 0, true): sets the score
    int value = parse_int0(s, v);
    p->score = value;
    if (p->score > GAME_SCORE_CAP) {
        p->score = GAME_SCORE_CAP;
        value = 0;
    }
    mode_on_score(p->game, p, value, 0);
    const NetScorePacket packet = {.delta = value, .score = p->score};
    game_send(p->game, p->uid, NET_PACKET_SCORE, &packet, sizeof(packet));
    p->game->leaders_dirty = 1;
}

static KsValue pl_lives(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_num(p->mode.lives > 0 ? p->mode.lives : 0); }
static void pl_lives_set(KsScript *s, void *native, uint32_t id, KsValue v)
{
    PLAYER_SET(r);
    if (!on_server(r)) return;
    int lives = parse_int0(s, v);
    if (lives < 0) lives = 0;
    if (p->mode.lives > 0 && lives == 0) player_kill(p, NULL, NULL, 0);
    else p->mode.lives = lives;
}

static KsValue pl_health(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; return ks_num(p->health); }
static void pl_health_set(KsScript *s, void *native, uint32_t id, KsValue v)
{
    PLAYER_SET(r);
    // parseInt(v), not a number -> unchanged; players.setHealth: godMode keeps it, "h" to all
    char buf[64];
    ks_to_string(s, v, buf, sizeof(buf));
    char *end = NULL;
    const double d = strtod(buf, &end);
    if (end == buf || !isfinite(d) || p->god_mode) return;
    p->health = (float) (d < 0 ? ceil(d) : floor(d));
    if (!game_is_authority(p->game)) return;
    const NetHealthPacket packet = {.uid = p->uid, .health = ceilf(p->health)};
    game_send(p->game, NET_TARGET_ALL, NET_PACKET_HEALTH, &packet, sizeof(packet));
    if (p->health <= 0.0f) player_kill(p, NULL, NULL, 0);
}

static KsValue pl_register_sync(KsScript *s, void *native, const KsValue *args, int argc, const int add)
{
    PLAYER_METHOD(r);
    if (!on_server(r)) return ks_bool(0);
    KsValue names = ARG(0);
    KsValue one[1];
    size_t count;
    if (names.tag == KS_STR) {
        one[0] = names;
        count = 1;
    } else if (names.tag == KS_ARR) {
        count = ks_arr_len(names);
    } else {
        return ks_bool(0);
    }
    for (size_t i = 0; i < count; i++) {
        const KsValue item = names.tag == KS_STR ? one[0] : ks_arr_get(s, names, i);
        char name[sizeof(r->sync[0])];
        const size_t n = ks_to_string(s, item, name, sizeof(name));
        // (name + "").length > 20 UTF-16 units: skipped
        size_t units = 0;
        for (size_t k = 0; k < n && k < sizeof(name) - 1; k++) {
            const unsigned char c = (unsigned char) name[k];
            if ((c & 0xC0) != 0x80) units += c >= 0xF0 ? 2 : 1;
        }
        if (n >= sizeof(name) - 1 || units > KS_SYNC_NAME_MAX) continue;
        if (add) {
            if (r->sync_count >= KS_SYNC_MAX_NAMES) return ks_bool(0);
            snprintf(r->sync[r->sync_count++], sizeof(r->sync[0]), "%s", name);
        } else {
            for (int k = 0; k < r->sync_count; k++) {
                if (strcmp(r->sync[k], name)) continue;
                memmove(r->sync[k], r->sync[k + 1], (size_t) (r->sync_count - k - 1) * sizeof(r->sync[0]));
                r->sync_count--;
                break;
            }
        }
    }
    (void) p;
    return ks_undef();
}
static KsValue pl_register(KsScript *s, void *native, const KsValue *args, int argc) { return pl_register_sync(s, native, args, argc, 1); }
static KsValue pl_deregister(KsScript *s, void *native, const KsValue *args, int argc) { return pl_register_sync(s, native, args, argc, 0); }

static KsValue pl_respawn(KsScript *s, void *native, const KsValue *args, int argc)
{
    (void) s;
    (void) args;
    (void) argc;
    PLAYER_METHOD(r);
    if (!on_server(r)) return ks_bool(0);
    if (p->mode.lives <= 0) p->mode.lives = 1;
    if (!p->active && p->game->hooks.respawn) p->game->hooks.respawn(p->game, p->uid); // "frs"
    return ks_undef();
}

// client-only members: inert in a server script (the client host replaces them)
static KsValue pl_false(KsScript *s, void *native, uint32_t id) { (void) s; PLAYER_OR(r, ks_undef()); (void) r; (void) p; return ks_bool(0); }
static void pl_noop_set(KsScript *s, void *native, uint32_t id, KsValue v) { (void) s; (void) native; (void) id; (void) v; }
static KsValue pl_noop(KsScript *s, void *native, const KsValue *args, int argc) { (void) s; (void) native; (void) args; (void) argc; return ks_undef(); }

// position / rotation / velocity sub-objects (non-replaceable, fields writable on both sides)
static float *vec_field(void *native, const int which, const int axis)
{
    Player *p = ks_player_of(native);
    if (!p) return NULL;
    switch (which) {
        case 0: return axis == 0 ? &p->position.x : axis == 1 ? &p->position.y : &p->position.z;
        case 1: return axis == 0 ? &p->velocity.x : axis == 1 ? &p->velocity.y : &p->velocity.z;
        default: return axis == 0 ? &p->direction.y : axis == 1 ? &p->direction.x : NULL; // rotation x = xDire (yaw), y = yDire (pitch)
    }
}
#define VEC_PROPS(prefix, which)                                                                                       \
    static KsValue prefix##_x(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; float *f = vec_field(n, which, 0); return f ? ks_num(*f) : ks_undef(); } \
    static KsValue prefix##_y(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; float *f = vec_field(n, which, 1); return f ? ks_num(*f) : ks_undef(); } \
    static KsValue prefix##_z(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; float *f = vec_field(n, which, 2); return f ? ks_num(*f) : ks_undef(); } \
    static void prefix##_sx(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; float *f = vec_field(n, which, 0); if (f) *f = (float) ks_to_float(v); } \
    static void prefix##_sy(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; float *f = vec_field(n, which, 1); if (f) *f = (float) ks_to_float(v); } \
    static void prefix##_sz(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; float *f = vec_field(n, which, 2); if (f) *f = (float) ks_to_float(v); }
VEC_PROPS(ppos, 0)
VEC_PROPS(pvel, 1)
VEC_PROPS(prot, 2)
#undef VEC_PROPS

static const KsHostProp g_player_position_props[] = {
    {"x", ppos_x, ppos_sx, NULL, 0}, {"y", ppos_y, ppos_sy, NULL, 0}, {"z", ppos_z, ppos_sz, NULL, 0}};
static const KsHostProp g_player_velocity_props[] = {
    {"x", pvel_x, pvel_sx, NULL, 0}, {"y", pvel_y, pvel_sy, NULL, 0}, {"z", pvel_z, pvel_sz, NULL, 0}};
static const KsHostProp g_player_rotation_props[] = {{"x", prot_x, prot_sx, NULL, 0}, {"y", prot_y, prot_sy, NULL, 0}};
static const KsHostClass g_player_position_class = {"position", g_player_position_props, 3};
static const KsHostClass g_player_velocity_class = {"velocity", g_player_velocity_props, 3};
static const KsHostClass g_player_rotation_class = {"rotation", g_player_rotation_props, 2};

static KsValue pl_sub(KsScript *s, void *native, const uint32_t id, const KsHostClass *cls)
{
    if (!ks_player_of(native)) return ks_undef();
    return ks_host_object(s, cls, native, id);
}
static KsValue pl_position(KsScript *s, void *n, uint32_t id) { return pl_sub(s, n, id, &g_player_position_class); }
static KsValue pl_velocity(KsScript *s, void *n, uint32_t id) { return pl_sub(s, n, id, &g_player_velocity_class); }
static KsValue pl_rotation(KsScript *s, void *n, uint32_t id) { return pl_sub(s, n, id, &g_player_rotation_class); }

#define KS_PLAYER_SHARED_PROPS                                                                                         \
    {"id", pl_id, NULL, NULL, 0},                                                                                      \
    {"sid", pl_sid, NULL, NULL, 0},                                                                                    \
    {"username", pl_username, NULL, NULL, 0},                                                                          \
    {"isYou", pl_is_you, NULL, NULL, 0},                                                                               \
    {"active", pl_active, NULL, NULL, 0},                                                                              \
    {"accountID", pl_account_id, NULL, NULL, 0},                                                                       \
    {"accountName", pl_account_name, NULL, NULL, 0},                                                                   \
    {"disableDefault", NULL, NULL, pl_disable_default, 0},                                                             \
    {"defaultMovement", pl_default_movement_get, pl_default_movement_set, NULL, 0},                                    \
    {"defaultVelocity", pl_default_velocity_get, pl_default_velocity_set, NULL, 0},                                    \
    {"defaultRotation", pl_default_rotation_get, pl_default_rotation_set, NULL, 0},                                    \
    {"disableShooting", pl_disable_shooting_get, pl_disable_shooting_set, NULL, 0},                                    \
    {"disableMelee", pl_disable_melee_get, pl_disable_melee_set, NULL, 0},                                             \
    {"assetID", pl_asset_id, pl_asset_id_set, NULL, 0},                                                                \
    {"onWall", pl_on_wall, pl_on_wall_set, NULL, 0},                                                                   \
    {"onGround", pl_on_ground, pl_on_ground_set, NULL, 0},                                                             \
    {"onTerrain", pl_on_terrain, pl_on_terrain_set, NULL, 0},                                                          \
    {"isCrouching", pl_crouching, pl_crouching_set, NULL, 0},                                                          \
    {"team", pl_team, NULL, NULL, 0},                                                                                  \
    {"classIndex", pl_class_index, pl_class_index_set, NULL, 0},                                                       \
    {"loadoutIndex", pl_loadout_index, pl_loadout_index_set, NULL, 0},                                                 \
    {"ammo", pl_ammo, NULL, NULL, 0},                                                                                  \
    {"changePrimary", NULL, NULL, pl_change_primary, 0},                                                               \
    {"changeSecondary", NULL, NULL, pl_change_secondary, 0},                                                           \
    {"giveWeapon", NULL, NULL, pl_give_weapon, 0},                                                                     \
    {"removePrimary", NULL, NULL, pl_remove_primary, 0},                                                               \
    {"removeSecondary", NULL, NULL, pl_remove_secondary, 0},                                                           \
    {"removeMelee", NULL, NULL, pl_remove_melee, 0},                                                                   \
    {"clearLoadout", NULL, NULL, pl_clear_loadout, 0},                                                                 \
    {"score", pl_score, pl_score_set, NULL, 0},                                                                        \
    {"lives", pl_lives, pl_lives_set, NULL, 0},                                                                        \
    {"rotation", pl_rotation, NULL, NULL, 1},                                                                          \
    {"position", pl_position, NULL, NULL, 1},                                                                          \
    {"velocity", pl_velocity, NULL, NULL, 1},                                                                          \
    {"health", pl_health, pl_health_set, NULL, 0},                                                                     \
    {"registerSyncValues", NULL, NULL, pl_register, 0},                                                                \
    {"deregisterSyncValues", NULL, NULL, pl_deregister, 0},                                                            \
    {"respawn", NULL, NULL, pl_respawn, 0}

const KsHostProp ks_player_props[] = {KS_PLAYER_SHARED_PROPS};
const int ks_player_prop_count = (int) (sizeof(ks_player_props) / sizeof(ks_player_props[0]));

static const KsHostProp g_player_all_props[] = {
    KS_PLAYER_SHARED_PROPS,
    {"visible", pl_false, pl_noop_set, NULL, 0},
    {"inView", pl_false, NULL, NULL, 0},
    {"getAsset", NULL, NULL, pl_noop, 0},
    {"playAnim", NULL, NULL, pl_noop, 0},
    {"stopAnim", NULL, NULL, pl_noop, 0},
};
const KsHostClass ks_player_class = {"player", g_player_all_props, (int) (sizeof(g_player_all_props) / sizeof(g_player_all_props[0]))};

KsValue ks_game_wrap_player(KsScript *s, Game *game, const Player *player)
{
    if (!s || !game || !player) return ks_null();
    KsRef *ref = ks_ref_get(ks_game_state(game, 1), KS_REF_PLAYER, player->uid);
    if (!ref) return ks_null();
    return ks_host_object(s, ks_game_player_class(game), ref, (uint32_t) player->uid);
}

// ---------------------------------------------------------------------------
// AI (3.2.2, API:554-625)

static GameAI *ai_of(void *native)
{
    KsRef *r = ref_of(native);
    if (!r || r->kind != KS_REF_AI) return NULL;
    return game_ai_find(ks_native_game(native), r->id);
}
static KsValue ai_sid(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; GameAI *a = ai_of(n); return a ? ks_num(a->sid) : ks_undef(); }
static KsValue ai_name(KsScript *s, void *n, uint32_t id) { (void) id; GameAI *a = ai_of(n); return a ? str_value(s, a->name) : ks_undef(); }
#ifdef GAME_AI_HAVE_V7_API // map-F's v7 AI.js port (shared/include/ai.h)
static KsValue ai_path_index(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; GameAI *a = ai_of(n); return a ? ks_num(a->path_index) : ks_undef(); }
#else
static KsValue ai_path_index(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; GameAI *a = ai_of(n); return a ? ks_num(0) : ks_undef(); }
#endif
static KsValue ai_behaviour(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; GameAI *a = ai_of(n); return a ? ks_num(a->behaviour_type) : ks_undef(); }
static void ai_behaviour_set(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; GameAI *a = ai_of(n); if (a) a->behaviour_type = (int) ks_to_float(v); }
static KsValue ai_health(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; GameAI *a = ai_of(n); return a ? ks_num(a->health) : ks_undef(); }
static void ai_health_set(KsScript *s, void *n, uint32_t id, KsValue v)
{
    (void) id;
    GameAI *a = ai_of(n);
    if (!a) return;
    char buf[64];
    ks_to_string(s, v, buf, sizeof(buf));
    char *end = NULL;
    const double d = strtod(buf, &end);
    if (end == buf || !isfinite(d)) return;
    Game *game = ks_native_game(n);
#ifdef GAME_AI_HAVE_V7_API
    game_ai_set_health(game, a, (float) (d < 0 ? ceil(d) : floor(d))); // changeHealth(v, null, false, true): set
#else
    a->health = (float) (d < 0 ? ceil(d) : floor(d)); // changeHealth(v, null, false, true): set
    if (a->health <= 0.0f && game_is_authority(game)) game_ai_kill(game, a, NULL);
#endif
}
// AI.playAnim(ai, clip, reps, raw) (server only; a string clip, reps default 1)
static KsValue ai_play_anim(KsScript *s, void *n, const KsValue *args, int argc)
{
    GameAI *a = ai_of(n);
    KsRef *r = ref_of(n);
    size_t len = 0;
    const char *clip = ks_str_utf8(ARG(0), &len);
    (void) s;
    if (!a || !clip || !r || r->kg->side != KS_SIDE_SERVER) return ks_undef();
#ifdef GAME_AI_HAVE_V7_API
    char name[64];
    snprintf(name, sizeof(name), "%.*s", (int) (len < sizeof(name) ? len : sizeof(name) - 1), clip);
    game_ai_play_anim(ks_native_game(n), a, name, 1);
#endif
    return ks_undef();
}
static float *ai_field(void *n, const int which, const int axis)
{
    GameAI *a = ai_of(n);
    if (!a) return NULL;
    if (which == 0) return axis == 0 ? &a->position.x : axis == 1 ? &a->position.y : &a->position.z;
    return axis == 0 ? &a->x_dir : axis == 1 ? &a->y_dir : NULL; // v7 bug (reads `player`) fixed: the AI's xD / yD
}
#define AI_VEC(prefix, which)                                                                                          \
    static KsValue prefix##_x(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; float *f = ai_field(n, which, 0); return f ? ks_num(*f) : ks_undef(); } \
    static KsValue prefix##_y(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; float *f = ai_field(n, which, 1); return f ? ks_num(*f) : ks_undef(); } \
    static KsValue prefix##_z(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; float *f = ai_field(n, which, 2); return f ? ks_num(*f) : ks_undef(); } \
    static void prefix##_sx(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; float *f = ai_field(n, which, 0); if (f) *f = (float) ks_to_float(v); } \
    static void prefix##_sy(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; float *f = ai_field(n, which, 1); if (f) *f = (float) ks_to_float(v); } \
    static void prefix##_sz(KsScript *s, void *n, uint32_t id, KsValue v) { (void) s; (void) id; float *f = ai_field(n, which, 2); if (f) *f = (float) ks_to_float(v); }
AI_VEC(apos, 0)
AI_VEC(arot, 1)
#undef AI_VEC
static const KsHostProp g_ai_position_props[] = {{"x", apos_x, apos_sx, NULL, 0}, {"y", apos_y, apos_sy, NULL, 0}, {"z", apos_z, apos_sz, NULL, 0}};
static const KsHostProp g_ai_rotation_props[] = {{"x", arot_x, arot_sx, NULL, 0}, {"y", arot_y, arot_sy, NULL, 0}};
static const KsHostClass g_ai_position_class = {"position", g_ai_position_props, 3};
static const KsHostClass g_ai_rotation_class = {"rotation", g_ai_rotation_props, 2};
static KsValue ai_position(KsScript *s, void *n, uint32_t id) { return ai_of(n) ? ks_host_object(s, &g_ai_position_class, n, id) : ks_undef(); }
static KsValue ai_rotation(KsScript *s, void *n, uint32_t id) { return ai_of(n) ? ks_host_object(s, &g_ai_rotation_class, n, id) : ks_undef(); }

static const KsHostProp g_ai_props[] = {
    {"sid", ai_sid, NULL, NULL, 0},
    {"displayName", ai_name, NULL, NULL, 0},
    {"position", ai_position, NULL, NULL, 1},
    {"rotation", ai_rotation, NULL, NULL, 1},
    {"pathIndex", ai_path_index, NULL, NULL, 0},
    {"behaviour", ai_behaviour, ai_behaviour_set, NULL, 0},
    {"health", ai_health, ai_health_set, NULL, 0},
    {"playAnim", NULL, NULL, ai_play_anim, 0},
};
const KsHostClass ks_ai_class = {"AI", g_ai_props, (int) (sizeof(g_ai_props) / sizeof(g_ai_props[0]))};

KsValue ks_game_wrap_ai(KsScript *s, Game *game, const int ai_sid)
{
    if (!s || !game || !game_ai_find(game, ai_sid)) return ks_undef();
    KsRef *ref = ks_ref_get(ks_game_state(game, 1), KS_REF_AI, ai_sid);
    return ref ? ks_host_object(s, &ks_ai_class, ref, (uint32_t) ai_sid) : ks_undef();
}

// ---------------------------------------------------------------------------
// Map objects (3.2.6 obj3DServ: sid, position, scale = width / height / length; read-only)

static Object *object_of(void *native)
{
    KsRef *r = ref_of(native);
    Game *game = ks_native_game(native);
    if (!r || r->kind != KS_REF_OBJECT || !game || !game->map || r->id < 0 || (size_t) r->id >= game->map->object_count) return NULL;
    return game->map->objects[r->id];
}
static KsValue ob_sid(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; return object_of(n) ? ks_num(ref_of(n)->id) : ks_undef(); }
#define OB_VEC(prefix, field)                                                                                          \
    static KsValue prefix##_x(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; Object *o = object_of(n); return o ? ks_num(o->field.x) : ks_undef(); } \
    static KsValue prefix##_y(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; Object *o = object_of(n); return o ? ks_num(o->field.y) : ks_undef(); } \
    static KsValue prefix##_z(KsScript *s, void *n, uint32_t id) { (void) s; (void) id; Object *o = object_of(n); return o ? ks_num(o->field.z) : ks_undef(); }
OB_VEC(opos, position)
OB_VEC(oscale, scale)
#undef OB_VEC
static const KsHostProp g_ob_position_props[] = {{"x", opos_x, NULL, NULL, 0}, {"y", opos_y, NULL, NULL, 0}, {"z", opos_z, NULL, NULL, 0}};
static const KsHostProp g_ob_scale_props[] = {{"x", oscale_x, NULL, NULL, 0}, {"y", oscale_y, NULL, NULL, 0}, {"z", oscale_z, NULL, NULL, 0}};
static const KsHostClass g_ob_position_class = {"position", g_ob_position_props, 3};
static const KsHostClass g_ob_scale_class = {"scale", g_ob_scale_props, 3};
static KsValue ob_position(KsScript *s, void *n, uint32_t id) { return object_of(n) ? ks_host_object(s, &g_ob_position_class, n, id) : ks_undef(); }
static KsValue ob_scale(KsScript *s, void *n, uint32_t id) { return object_of(n) ? ks_host_object(s, &g_ob_scale_class, n, id) : ks_undef(); }
static const KsHostProp g_object_props[] = {
    {"sid", ob_sid, NULL, NULL, 0},
    {"position", ob_position, NULL, NULL, 1},
    {"scale", ob_scale, NULL, NULL, 1},
};
const KsHostClass ks_map_object_class = {"object", g_object_props, 3};

KsValue ks_game_wrap_map_object(KsScript *s, Game *game, const int object_index)
{
    if (!s || !game || !game->map || object_index < 0 || (size_t) object_index >= game->map->object_count) return ks_undef();
    KsRef *ref = ks_ref_get(ks_game_state(game, 1), KS_REF_OBJECT, object_index);
    return ref ? ks_host_object(s, &ks_map_object_class, ref, (uint32_t) object_index) : ks_undef();
}

// ---------------------------------------------------------------------------
// GAME.* entries

static Game *api_game(void *user) { return user ? ((KsGame *) user)->game : NULL; }

static KsValue api_players_list(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) args;
    (void) argc;
    Game *game = api_game(u);
    KsValue list = ks_arr_new(s);
    if (!game) return list;
    for (size_t i = 0; i < game->player_count; i++)
        if (game->players[i]) ks_arr_push(s, list, ks_game_wrap_player(s, game, game->players[i]));
    return list;
}

static KsValue api_players_find(KsScript *s, void *u, const KsValue *args, int argc)
{
    Game *game = api_game(u);
    Player *p = ks_game_player_by_id(game, ARG(0));
    return p ? ks_game_wrap_player(s, game, p) : ks_null();
}

static KsValue api_objects_list(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) args;
    (void) argc;
    Game *game = api_game(u);
    KsValue list = ks_arr_new(s);
    if (!game || !game->map) return list;
    for (size_t i = 0; i < game->map->object_count; i++) ks_arr_push(s, list, ks_game_wrap_map_object(s, game, (int) i));
    return list;
}

// OBJECTS.getPathNodes: {id, x, y, z} of manager.pathNodes (plain read-only values)
static KsValue api_path_nodes(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) args;
    (void) argc;
    Game *game = api_game(u);
    KsValue list = ks_arr_new(s);
    if (!game || !game->map) return list;
    for (size_t i = 0; i < game->map->path_node_count; i++) {
        const MapPathNode *node = &game->map->path_nodes[i];
        if (node->object >= game->map->object_count) continue;
        const Object *o = game->map->objects[node->object];
        KsValue v = ks_obj_new(s);
        ks_obj_set(s, v, "id", ks_num(node->nid));
        ks_obj_set(s, v, "x", ks_num(o->position.x));
        ks_obj_set(s, v, "y", ks_num(o->position.y));
        ks_obj_set(s, v, "z", ks_num(o->position.z));
        ks_arr_push(s, list, v);
    }
    return list;
}

static KsValue api_false(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) s; (void) u; (void) args; (void) argc;
    return ks_bool(0);
}

// NFT.ownedAssets (server): no wallets in KN -> f2125("Missing wallet or player"), no callback
static KsValue api_owned_assets(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) s; (void) args; (void) argc;
    KsGame *kg = u;
    if (kg && kg->host && kg->host->log) kg->host->log(kg->host_user, 1, "Missing wallet or player");
    return ks_undef();
}

// AI.spawn(assetID, name, x, y, z, data) (API:3143-3168, options 3.3.3)
#define KS_AI_LIMIT 40 // config.aiLimit (CFG:2328)

static double opt_num(KsScript *s, const KsValue data, const char *key, const double lo, const double hi)
{
    const KsValue v = ks_obj_get(s, data, key);
    double d = v.tag == KS_NUM ? v.as.num : ks_to_float(v); // missing -> 0, then clamped
    if (!isfinite(d)) d = 0.0;
    return d < lo ? lo : d > hi ? hi : d;
}
static int opt_bool(KsScript *s, const KsValue data, const char *key) { return truthy(ks_obj_get(s, data, key)); }

static KsValue api_ai_spawn(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = u;
    Game *game = api_game(u);
    if (!kg || kg->side != KS_SIDE_SERVER || !game || !game_is_authority(game)) return ks_undef();
    GameAIs *list = game_ais(game);
    int alive = 0;
    for (int i = 0; list && i < list->count; i++) alive += list->ais[i].active || list->ais[i].respawn_timer > 0.0f;
    if (!list || alive >= KS_AI_LIMIT) return ks_undef();

    KsValue data = ARG(5);
    if (data.tag != KS_OBJ) data = ks_obj_new(s);
    GameAIData d;
    memset(&d, 0, sizeof(d));
#ifdef GAME_AI_HAVE_V7_API
    game_ai_data_defaults(&d); // AI.init defaults; the option keys below overwrite them like vO445.AI
#endif
    snprintf(d.src, sizeof(d.src), "model");
    char asset[64];
    ks_to_string(s, ARG(0), asset, sizeof(asset));
    d.asset = atoi(asset);
    d.health = (float) ks_to_float(ks_obj_get(s, data, "health"));
    if (d.health <= 0.0f) d.health = 100.0f;
    d.respawn_t = (float) ks_to_float(ks_obj_get(s, data, "respawnT"));
    d.respawn_r = (unsigned char) opt_bool(s, data, "respawnR");
    d.can_shoot = (unsigned char) opt_bool(s, data, "canShoot");
    d.can_hit = (unsigned char) opt_bool(s, data, "canMelee");
    d.speed = (float) opt_num(s, data, "speed", 0, 10);
    d.turn_speed = (float) opt_num(s, data, "turnSpeed", 0, 10);
    d.gravity = (float) opt_num(s, data, "gravity", 0, 1);
    d.fire_rate = (int) opt_num(s, data, "fireRate", 200, 10000);
    d.vision = (int) opt_num(s, data, "visionDis", 1, 1000);
    d.score = (int) opt_num(s, data, "score", 0, 10000);
    d.behaviour = (int) opt_num(s, data, "behaviorType", 0, 20);
    d.m_size = (float) opt_num(s, data, "modelScale", 0.01, 100);
    char name[GAME_AI_NAME_SIZE];
    ks_to_string(s, ARG(1), name, sizeof(name));
    const vec3 pos = {(float) ks_to_float(ARG(2)), (float) ks_to_float(ARG(3)), (float) ks_to_float(ARG(4))};
    GameAI *ai = game_ai_spawn(game, 0, 0, name, pos, &d);
    return ai ? ks_game_wrap_ai(s, game, ai->sid) : ks_undef();
}

static KsValue api_ai_remove(KsScript *s, void *u, const KsValue *args, int argc)
{
    KsGame *kg = u;
    Game *game = api_game(u);
    if (!kg || kg->side != KS_SIDE_SERVER || !game) return ks_undef();
    GameAI *ai = game_ai_find(game, parse_int0(s, ARG(0)));
    if (ai) { // AI.removeFully: gone for good
#ifdef GAME_AI_HAVE_V7_API
        game_ai_remove(game, ai);
#else
        ai->active = 0;            // no respawn, dropped from the next sync
        ai->respawn_timer = 0.0f;
#endif
    }
    return ks_undef();
}

static KsValue api_ai_list(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void) args;
    (void) argc;
    KsGame *kg = u;
    Game *game = api_game(u);
    if (!kg || kg->side != KS_SIDE_SERVER || !game) return ks_undef();
    KsValue list = ks_arr_new(s);
    GameAIs *ais = game->ai;
    // AI.ais: an AI removed this tick (game_ai_remove, dropped at the next AI tick) is no longer listed
    for (int i = 0; ais && i < ais->count; i++)
        if (game_ai_find(game, ais->ais[i].sid)) ks_arr_push(s, list, ks_game_wrap_ai(s, game, ais->ais[i].sid));
    return list;
}

void ks_hostobj_api_fill(KsApiImpl *api, const KsSide side)
{
    api[KS_API_PLAYERS_LIST].fn = api_players_list;
    api[KS_API_PLAYERS_FINDBYID].fn = api_players_find;
    api[KS_API_OBJECTS_LIST].fn = api_objects_list;
    api[KS_API_OBJECTS_GETPATHNODES].fn = api_path_nodes;
    api[KS_API_NFT_HASWALLET].fn = api_false;
    if (side == KS_SIDE_SERVER) {
        api[KS_API_PLATFORM_ISCLIENT].fn = api_false;
        api[KS_API_PLATFORM_ISBROWSER].fn = api_false;
        api[KS_API_PLATFORM_ISMOBILE].fn = api_false;
        api[KS_API_NFT_OWNEDASSETS].fn = api_owned_assets;
        api[KS_API_AI_SPAWN].fn = api_ai_spawn;
        api[KS_API_AI_REMOVE].fn = api_ai_remove;
        api[KS_API_AI_LIST].fn = api_ai_list;
    }
}
