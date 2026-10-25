// Krunker v3.1.1 sound triggers, derived from per-frame player state diffs.
//
// Sources (Krunker.Unpacked-v3.1.1): 95.js player manager (shoot -> playSound(weapon.sound,
// 0.85, rate 0.9-1.1); melee swsh_*; reload reload_1 / endReload reload_2 at half time;
// hvJK5 aim_0/aim_1; swap melee_equip; jump jump_0/1; hMY7 land step_N 0.7 + slide_0;
// Te7 footsteps; bounce_0 on jump pads; casings case_N), 106.js (map ambience
// "ambient_" + ambInd, playerSound range checks), 94.js/93.js (SOUND_EMITTER play3DL),
// index.js (gF hit_0, gk kill sound, impact_0 when the HUD health drops).
//
// Only the local player runs physics on the client, so - as in Krunker, where those
// call sites are 2D SOUND.play on the client - jump/land/aim/reload/step sounds are local;
// other players get positional footsteps (Krunker computes those client-side as well).
#include <sound_events.h>
#include <class_camera.h> // the listener at the head
#include <audio.h>
#include <client_settings.h> // Settings > Game > Gameplay "Always Play HS Sound on Kill"
#include <cosmetics.h>
#include <stalker.h> // v10 Stalker: silent steps
#include <defuse.h>  // v10 Defuse: the planter's beeps
#include <piss.h> // KrunkNative Piss: PISS_SOUND_INTERVAL
#include <nettspend_audio.h> // KrunkNative Nettspend: the Drank's pour up, Money Spread's throw, the laced loop
#include <network_sounds.h> // networkSounds: the names a shot may play
#include "viewer/viewer_store.h"
#include <math.h>
#include <pcg_basic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// shared/src/player.c (not in a header): the simulation's own collision test, used to tell
// a jump pad launch from a jump.
int player_collides(Player *player, const Object *object, float pad);

#define STEP_MLT 0.25f          // Krunker config stepMlt
#define STEP_RANGE 170.0f       // playerSound range for other players' steps
#define SHELL_INTERVAL 0.4f     // a0.lastShell throttle (400 ms)
#define BOUNCE_INTERVAL 0.25f   // one bounce_0 per pad launch (Krunker plays it per colliding frame)
#define TELEPORT_DISTANCE 20.0f // per-frame moves above max(this, TELEPORT_SPEED * delta) are
#define TELEPORT_SPEED 400.0f   // spawns/teleports, not steps (units, units/s)
#define MAX_LOADOUT (NET_LOADOUT_MAX + PLAYER_EXTRA_WEAPONS_MAX) // the class loadout + Object Manager extra slots
#define MAX_REMOTE 64

typedef struct {
    const Player *player;
    int uid;
    int valid;
    int seen;

    int loadout_index;
    const Weapon *weapon;
    float reloads[MAX_LOADOUT];
    float reload_timer;
    float aim_val;
    float health;
    float velocity_y;
    vec3 position;
    unsigned char on_ground, can_slide, can_throw;

    float step_val;
    float step_dir;
    unsigned short step_events, land_events; // Player.step_events / land_events last seen (local player)
    float last_shell;
    float last_bounce;
    float last_spray; // KrunkNative Piss: the last spray sound (PISS_SOUND_INTERVAL)
    unsigned char inspecting; // Player.inspecting last seen (the local player's inspect sound)
} PlayerTrack;

static struct {
    PlayerTrack me;
    PlayerTrack remote[MAX_REMOTE];
    const Map *map;
    int map_index;
    int ambient;            // ambient_N currently looping, 0 = none
    int preloaded;
    float now; // sound_events_tick's clock (the remote Piss spray throttle)
    float plant_tick; // Defuse: interTick, the planter's beep_0 countdown (s)
    char inspect_sound[96]; // SOUND.localInspectSound: the inspect sound playing, "" none
} g_sound;

// weapon.sound per weapon (v7.1.4 src/data/weapons.js; Weapon.sound carries the same name), whether it
// ejects casings (!nCase) and its swapSnd (the Build Tool's paper_1, deob-10.0.3.js:114368, :201760). Matched on Weapon.name.
static const struct {
    const char *weapon;
    const char *sound;
    int casings;
    const char *swap_snd;
} WEAPON_SOUNDS[] = {
    {"Sniper Rifle", "weapon_1", 1},
    {"Assault Rifle", "weapon_2", 1},
    {"Pistol", "weapon_3", 1},
    {"Submachine Gun", "weapon_4", 1},
    {"Revolver", "weapon_5", 0},
    {"Shotgun", "weapon_6", 0},
    {"Machine Gun", "weapon_7", 1},
    {"Semi Auto", "weapon_8", 1},
    {"Rocket Launcher", "weapon_9", 0},
    {"Akimbo Uzi", "weapon_10", 1},
    {"Desert Eagle", "weapon_11", 1},
    {"Alien Blaster", "weapon_12", 0},
    {"Crossbow", "weapon_14", 0},
    {"Famas", "weapon_15", 1},
    {"Sawed Off", "weapon_16", 0}, // v7.1.4: weapon_6 -> weapon_16
    {"Auto Pistol", "weapon_17", 1},
    {"Bomb", "weapon_18", 0},
    {"Blaster", "weapon_19", 0},
    // v7.1.4 weapons 19-28 (WP-B)
    {"Build Tool", "weapon_20", 0, "paper_1"},
    {"Grappler", "weapon_21", 1},
    {"Tehchy-9", "weapon_22", 1},
    {"Noob Tube", "weapon_23", 0},
    {"Slimer", "weapon_24", 1},
    {"Zapper", "weapon_25", 1},
    {"Minigun", "weapon_26", 1},
    {"War Machine", "weapon_27", 0},
    {"Akimbo Pistol", "weapon_28", 1},
    {"Charge Rifle", "weapon_29", 1},
    // v10.0.2 (only in the v10 ruleset's table): the Boulder never fires (sound ''), the Compressor
    // has nCase (no casings) (v10:101808-101944)
    {"Compressor", "weapon_31", 0},
};

// Krunker's amvVols: per-ambience volume, 0.12 otherwise.
static float ambient_volume(const int index) {
    return index == 6 ? 0.3f : index == 7 ? 0.4f : 0.12f;
}

static float rand_float(const float min, const float max) {
    return min + (max - min) * (float) ((double) pcg32_random() / (double) UINT32_MAX);
}

static int rand_int(const int min, const int max) {
    return min + (int) (pcg32_random() % (uint32_t) (max - min + 1));
}

static int weapon_entry(const Weapon *weapon) {
    if (!weapon || !weapon->name) return -1;
    for (size_t i = 0; i < sizeof(WEAPON_SOUNDS) / sizeof(WEAPON_SOUNDS[0]); i++) {
        if (!strcmp(WEAPON_SOUNDS[i].weapon, weapon->name)) return (int) i;
    }
    return -1;
}

// shoot()'s sound (deob-10.0.3.js:196392-196405) for the slot `p` holds: weapon.sound ("weapon_upgraded" on a
// krumMachine-upgraded slot: upgradeWeapons' sound, deob:202656), + "_blast" for a full charge, else + "_<mid>" for the
// remodel the slot wears, its store skin (getSkinForLoadout) or else the weapon's altSkin, unless noSnd
// (viewer_weapon_shot_sound). v10 plays networkSounds[indexOf(name)], so a name outside that table plays nothing. -1: no
// shot sound for this weapon (outside Krunker's table); 0: silent; 1: play `name`. *casings: the weapon's !nCase.
static int shot_sound(const Player *p, const int blast, char *name, const size_t n, int *casings) {
    name[0] = 0;
    if (!p->weapon || !p->loadout || p->loadout_index < 0 || p->loadout_index >= p->loadout_size) return -1;
    const int slot = p->loadout_index, id = p->loadout[slot];
    const int upgraded = slot < 8 && (p->upgraded_slots >> slot & 1);
    const Weapon *base = upgraded && p->game && p->game->weapons && id >= 0 && (size_t) id < p->game->weapon_count ? p->game->weapons[id] : p->weapon;
    const int entry = weapon_entry(base); // an upgraded entry may carry another name (the AWP Blaster)
    if (entry < 0) return -1;
    if (casings) *casings = WEAPON_SOUNDS[entry].casings;
    const ViewerSkin *skin = viewer_skin(viewer_loadout_skin(p->skins.primary, p->skins.secondary, id));
    // 2: a custom skin's customSound, played from its user asset (SOUND.play isAsset, not networkSounds)
    if (viewer_weapon_shot_sound(upgraded ? "weapon_upgraded" : WEAPON_SOUNDS[entry].sound, skin, id, upgraded, blast, name, n) == 2) return 1;
    return name[0] && network_sound_index(name) >= 0;
}

// players.currentSkin / currentMelee (deob-10.0.3.js:190469-190496): the melee skin on a melee weapon, else the store skin
// getSkinForLoadout finds for the held slot (none on an upgraded slot). The swap and inspect sounds read it.
static const ViewerSkin *current_skin(const Player *p) {
    if (!p->weapon || !p->loadout || p->loadout_index < 0 || p->loadout_index >= p->loadout_size) return NULL;
    if (p->weapon->melee) return viewer_skin(p->skins.melee);
    const int slot = p->loadout_index;
    if (slot < 8 && (p->upgraded_slots >> slot & 1)) return NULL;
    return viewer_skin(viewer_loadout_skin(p->skins.primary, p->skins.secondary, p->loadout[slot]));
}

// getReloadSnd's skin: player.skins[loadoutIndex] (deob-10.0.3.js:198752: by the slot's index, skins[0] / skins[1])
static const ViewerSkin *slot_index_skin(const Player *p) {
    const int slot = p->loadout_index;
    return viewer_skin(slot == 0 ? p->skins.primary : slot == 1 ? p->skins.secondary : -1);
}

static void play_numbered(const char *prefix, const int min, const int max, const float volume, const float rate) {
    char name[32];
    snprintf(name, sizeof(name), "%s%d", prefix, rand_int(min, max));
    audio_play(name, volume, 0, rate);
}

// v7.1.4 players.js getStepSound / getJumpSound: a player wearing shoes with stepSnd steps and lands
// with "shoe_<id>" (and jumps with it when they also have jumpSnd); NULL = the default sounds.
static const ViewerSkin *step_shoes(const Player *player) {
    PlayerCosmetics cosmetics = player->cosmetics;
    PlayerCosmeticsV7 v7 = player->cosmetics_v7;
    cosmetics_route(&cosmetics, &v7);
    const ViewerSkin *shoes = cosmetic_item(v7.shoe, ITEM_SHOE);
    return shoes && (shoes->flags2 & VS2_STEP_SND) ? shoes : NULL;
}

// a footstep (Te7): step_N, or the shoes' sound at 0.6 x the volume and a 0.95-1.05 rate
static void play_step(const Player *player, const int positional, float volume, float rate) {
    char name[32];
    const ViewerSkin *shoes = step_shoes(player);
    if (shoes) {
        snprintf(name, sizeof(name), "shoe_%d", shoes->id);
        volume *= 0.6f;
        rate *= rand_float(0.95f, 1.05f);
    } else {
        snprintf(name, sizeof(name), "step_%d", rand_int(0, 2));
    }
    if (positional) audio_play_3d(name, player->position, volume, rate, 0.0f, 0.0f);
    else audio_play(name, volume, 0, rate);
}

static float distance3(const vec3 a, const vec3 b) {
    const float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

static float distance2(const vec3 a, const vec3 b) {
    return hypotf(a.x - b.x, a.z - b.z);
}

// ---------------------------------------------------------------- map ambience / emitters

static int map_ambient_index(const Map *map) {
    const cJSON *amb = map && map->raw_data ? cJSON_GetObjectItem(map->raw_data, "ambInd") : NULL;
    int index = 0;
    if (cJSON_IsNumber(amb)) index = amb->valueint;
    else if (cJSON_IsString(amb) && amb->valuestring) index = atoi(amb->valuestring); // parseInt
    return index > 0 ? index : 1;
}

// map.js SOUND_EMITTER -> MapManager.addEmitter -> SOUND.play3DL (deob-10.0.3.js:189196-189204,
// :186572-186580): the sound is config.emitterSounds[es] (v10.0.3's 74; an index past it plays
// emitterSounds[0]) or the user asset esa ("asset:<id>", user-assets/<id>/sound.mp3), with ev / er /
// ed / ero / edm under the emitter limits (shared/include/triggers.h trigger_emitter_parse). Old maps'
// "es" indices are the same list (shared/src/triggers.c).
static void map_start_emitters(const Map *map) {
    const cJSON *objects = map && map->raw_data ? cJSON_GetObjectItem(map->raw_data, "objects") : NULL;
    const cJSON *object;

    cJSON_ArrayForEach(object, objects) {
        const cJSON *id = cJSON_GetObjectItem(object, "i");
        if (!cJSON_IsNumber(id) || id->valueint != PREFAB_SOUND_EMITTER) continue;

        const cJSON *p = cJSON_GetObjectItem(object, "p");
        if (!cJSON_IsArray(p) || cJSON_GetArraySize(p) < 3) continue;

        const vec3 position = {
            (float) cJSON_GetArrayItem(p, 0)->valuedouble,
            (float) cJSON_GetArrayItem(p, 1)->valuedouble,
            (float) cJSON_GetArrayItem(p, 2)->valuedouble,
        };

        TriggerEmitter emitter;
        if (!trigger_emitter_parse(object, &emitter)) continue;
        // play3DL's pannerAttr: refDistance dist || 25, rolloffFactor roll || 1 (audio.c applies the defaults to 0),
        // distanceModel ["inverse", "linear", "exponential"][distM] (deob-10.0.3.js:225418-225421)
        audio_play_3d_loop(emitter.sound, position, emitter.volume, emitter.rate, emitter.distance, emitter.rolloff, emitter.distance_model);
    }
}

// 106.js game init: `H.play("ambient_" + ambInd, amvVols[ambInd] || 0.12, true, 1)`, the
// previous ambience stopped only when the index changes.
// WP-C "camb" (v7 window.changeAmbient): the ambience loop switches when the index changes.
void sound_events_change_ambient(const int ambient) {
    if (ambient <= 0 || ambient == g_sound.ambient) return;
    char name[32];
    if (g_sound.ambient) {
        snprintf(name, sizeof(name), "ambient_%d", g_sound.ambient);
        audio_stop(name);
    }
    g_sound.ambient = ambient;
    snprintf(name, sizeof(name), "ambient_%d", ambient);
    audio_play(name, ambient_volume(ambient), 1, 1.0f); // SOUND.play("ambient_" + id, amvVols[id] || 0.12, true, 1)
}

static void map_sounds_update(Client *client) {
    const Map *map = client->game.ready ? client->game.map : NULL;
    if (map == g_sound.map && client->game.current_map_index == g_sound.map_index) return;

    g_sound.map = map;
    g_sound.map_index = client->game.current_map_index;
    audio_stop_3d_loops();
    nettspend_audio_reset(); // KrunkNative Nettspend: a laced state does not outlive its map
    if (!map) return;

    const int ambient = map_ambient_index(map);
    char name[32];

    if (ambient != g_sound.ambient) {
        if (g_sound.ambient) {
            snprintf(name, sizeof(name), "ambient_%d", g_sound.ambient);
            audio_stop(name);
        }
        g_sound.ambient = ambient;
    }

    snprintf(name, sizeof(name), "ambient_%d", ambient);
    audio_play(name, ambient_volume(ambient), 1, 1.0f); // no-op while that loop already plays

    map_start_emitters(map);
}

// ---------------------------------------------------------------- tracking

static void track_reset(PlayerTrack *track, const Player *player) {
    memset(track, 0, sizeof(*track));
    track->player = player;
    track->uid = player->uid;
    track->valid = 1;
    track->step_dir = 1.0f;
    track->last_shell = -1000.0f;
    track->last_bounce = -1000.0f;
}

static void track_store(PlayerTrack *track, const Player *player) {
    track->loadout_index = player->loadout_index;
    track->weapon = player->weapon;
    for (int i = 0; i < MAX_LOADOUT; i++) {
        track->reloads[i] = player->reloads && i < player->loadout_size ? player->reloads[i] : 0.0f;
    }
    track->reload_timer = player->reload_timer;
    track->aim_val = player->aim_val;
    track->health = player->health;
    track->velocity_y = player->velocity.y;
    track->position = player->position;
    track->on_ground = player->on_ground;
    track->can_slide = player->can_slide;
    track->can_throw = player->can_throw;
    track->step_events = player->step_events;
    track->land_events = player->land_events;
    track->inspecting = player->inspecting;
}

// Te7: the step phase advances with the distance moved; a step sounds at each +-PI/2.
static int step_advance(PlayerTrack *track, const float distance) {
    track->step_val += distance * STEP_MLT * track->step_dir;
    const float half_pi = (float) M_PI / 2.0f;
    if ((track->step_val >= half_pi && track->step_dir > 0.0f) || (track->step_val <= -half_pi && track->step_dir < 0.0f)) {
        track->step_dir = -track->step_dir;
        return 1;
    }
    return 0;
}

static int on_jump_pad(const Player *player) {
    const Map *map = player->game ? player->game->map : NULL;
    if (!map) return 0;
    for (size_t i = 0; i < map->object_count; i++) {
        const Object *object = map->objects[i];
        if (object && object->active && object->jump_pad &&
            player_collides((Player *) player, object, game_constants.collision_padding)) return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- local player

static void local_shot(PlayerTrack *track, const Player *me, const float now) {
    const Weapon *weapon = me->weapon;

    if (weapon->melee) {
        // melee(): swsh_<skin id or 0>_<0|1> while the knife can be thrown, else swsh_def_*.
        play_numbered(track->can_throw ? "swsh_0_" : "swsh_def_", 0, 1, 0.1f, 1.0f);
        return;
    }

    if (weapon->build) return; // players.build sets the fire timer without a sound

    // KrunkNative Nettspend: Money Spread's cash throw (by weapon id, it is outside Krunker's table)
    if (me->loadout && nettspend_audio_shot(me->loadout[me->loadout_index])) return;

    // KrunkNative Piss (fireMustard): SOUND.play("spray", 0.12, rate 0.85-1.08) at most every 220 ms
    if (weapon->stream) {
        // KrunkNative Nettspend: the Drank's stream splashes (nettspend_audio_stream_sound), its Weapon.sound is the pour
        const char *stream_sound = me->loadout ? nettspend_audio_stream_sound(me->loadout[me->loadout_index]) : NULL;
        if (!stream_sound) stream_sound = weapon->sound;
        if (stream_sound && now - track->last_spray >= PISS_SOUND_INTERVAL) {
            track->last_spray = now;
            audio_play(stream_sound, 0.12f, 0, rand_float(0.85f, 1.08f));
        }
        return;
    }

    // shoot(): playSound(fullCharge ? weapon.sound + "_blast" : weapon.sound (+ the remodel's "_<mid>"), 0.85, ..., rate
    // randFloat(0.9, 1.1)) -> SOUND.play (shot_sound); tryGrapple plays weapon.sound the same way.
    char name[48];
    int casings = 0;
    const int shot = shot_sound(me, me->shot_blast && weapon->full_charge_shot, name, sizeof(name), &casings);
    if (shot < 0) return;
    if (shot) audio_play(name, 0.85f, 0, rand_float(0.9f, 1.1f));

    if (casings && !(weapon->grapple_range > 0.0f) && now - track->last_shell > SHELL_INTERVAL) {
        track->last_shell = now;
        play_numbered("case_", 0, 1, 0.45f, rand_float(0.8f, 1.3f));
    }
}

static void local_player_events(Client *client, const float now, const float delta) {
    Player *me = client->me;
    PlayerTrack *track = &g_sound.me;

    if (!me || !me->active || !me->weapon || !client->game.ready) {
        track->valid = 0;
        g_sound.plant_tick = 0.0f; // interTick = 0 on the next life
        return;
    }

    if (!track->valid || track->player != me || track->uid != me->uid) {
        track_reset(track, me);
        track_store(track, me);
        return;
    }

    const int same_weapon = me->loadout_index == track->loadout_index && me->weapon == track->weapon;
    const int index = me->loadout_index;

    // Shots / melee swings: the fire cooldown only ever jumps up when a shot was taken.
    if (same_weapon && index >= 0 && index < MAX_LOADOUT && me->reloads &&
        me->reloads[index] > track->reloads[index] + 1e-6f) {
        local_shot(track, me, now);
    }

    // reload(): getReloadSnd(player, 1) at 0.25 when it starts; endReload(): getReloadSnd(player, "2") when the timer runs
    // out (v10.0.3 player.update, deob-10.0.3.js:192306-192308; v7.1.4 played it at half time). getReloadSnd
    // (:198751-198758): reload_<n>, or a reloadSnd skin's weapon.sound_<mid>_r_<n> (Bondsman, Ad Astra, Monte Carlo...).
    // KrunkNative Nettspend: the Purple Drank's reload is the "pour up" (nettspend_audio.c: the
    // pour, the scream and the line fill the whole reload), no reload_1 / reload_2.
    const int drank = me->loadout && index >= 0 && index < me->loadout_size && nettspend_is_drank(me->loadout[index]);
    char reload_name[64];
    if (same_weapon && me->reload_timer > 0.0f && track->reload_timer <= 0.0f) {
        viewer_skin_reload_sound(me->weapon->sound, slot_index_skin(me), 1, reload_name, sizeof(reload_name));
        if (!nettspend_audio_reload_start(drank ? me->loadout[index] : -1)) audio_play(reload_name, 0.25f, 0, 1.0f);
    }
    if (same_weapon && !drank && track->reload_timer > 0.0f && me->reload_timer <= 0.0f) {
        viewer_skin_reload_sound(me->weapon->sound, slot_index_skin(me), 2, reload_name, sizeof(reload_name));
        audio_play(reload_name, 0.25f, 0, 1.0f);
    }

    // Swap (swapWeapon, deob-10.0.3.js:198261-198280): the skin's customSwapSound, else weapon.swapSnd ?? skin.swapSnd (volume
    // swapSndVol ?? 1 / 0.05); without one, melee_equip when the throwable knife comes out (not while aiming).
    if (!same_weapon) {
        const int entry = weapon_entry(me->weapon);
        char swap_name[96];
        float swap_volume = 0.05f;
        const int swap = viewer_skin_swap_sound(entry >= 0 ? WEAPON_SOUNDS[entry].swap_snd : NULL, 0.0f, current_skin(me), swap_name,
                                                sizeof(swap_name), &swap_volume);
        if (swap) audio_play(swap_name, swap_volume, 0, 1.0f);
        else if (me->weapon->melee && me->can_throw && track->aim_val == 0.0f) audio_play("melee_equip", 0.06f, 0, 1.5f);
    }

    // Inspect (wInspect / cancelInspect, deob-10.0.3.js:195531-195540, :195578-195584): the inspected skin's
    // customInspectSound or inspS (volume 1, looping with inspSLoop) as SOUND.localInspectSound, which a cancel stops.
    // Not ported: a flipAnim melee's melee_equip on inspect (no flipAnim in the store table).
    if (me->inspecting && !track->inspecting) {
        if (g_sound.inspect_sound[0]) audio_stop(g_sound.inspect_sound);
        int loop = 0;
        if (viewer_skin_inspect_sound(current_skin(me), g_sound.inspect_sound, sizeof(g_sound.inspect_sound), &loop))
            audio_play(g_sound.inspect_sound, 1.0f, loop, 1.0f);
    } else if (!me->inspecting && track->inspecting && g_sound.inspect_sound[0]) {
        audio_stop(g_sound.inspect_sound);
        g_sound.inspect_sound[0] = 0;
    }

    // hvJK5: aim_1 when fully scoped in, aim_0 when leaving full scope (not for melee).
    if (same_weapon && !me->weapon->melee) {
        if (me->aim_val >= 1.0f && track->aim_val < 1.0f) audio_play("aim_1", 0.1f, 0, 1.0f);
        else if (me->aim_val < 1.0f && track->aim_val >= 1.0f) audio_play("aim_0", 0.1f, 0, 1.0f);
    }

    // Upward launch: jump pad (bounce_0) or jump / wall jump (jump_0|1 at 0.04).
    if (me->velocity.y > 0.02f && me->velocity.y - track->velocity_y > 0.02f) {
        if (on_jump_pad(me)) {
            if (now - track->last_bounce > BOUNCE_INTERVAL) {
                track->last_bounce = now;
                audio_play("bounce_0", 1.0f, 0, rand_float(0.95f, 1.0f));
            }
        } else {
            const ViewerSkin *shoes = step_shoes(me);
            if (shoes && (shoes->flags2 & VS2_JUMP_SND)) { // getJumpSound: stepSnd && jumpSnd
                char name[32];
                snprintf(name, sizeof(name), "shoe_%d", shoes->id);
                audio_play(name, 0.04f, 0, 1.0f);
            } else {
                play_numbered("jump_", 0, 1, 0.04f, 1.0f);
            }
        }
    }

    const float moved = distance3(me->position, track->position);
    const float teleport = fmaxf(TELEPORT_DISTANCE, TELEPORT_SPEED * delta);

    // hMY7 (landing): step_N at 0.7 per resetStep the simulation counted (player.c), plus
    // slide_0 when the landing starts a slide.
    (void) teleport;
    // v10: a stalker lands, slides and steps without a sound (`!player.isStalker`, deob-10.0.3.js:195189,
    // :195226); everyone else's footsteps stay
    const int stalker = player_is_stalker(&client->game, me);
    if (me->land_events != track->land_events && !stalker) { // resetStep: getStepSound at (stepSnd ? 0.4 : 1) * 0.7
        const ViewerSkin *shoes = step_shoes(me);
        if (shoes) {
            char name[32];
            snprintf(name, sizeof(name), "shoe_%d", shoes->id);
            audio_play(name, 0.4f * 0.7f, 0, 1.0f);
        } else {
            play_numbered("step_", 0, 2, 0.7f, 1.0f);
        }
    }
    if (track->can_slide && !me->can_slide && me->crouch_val > 0.0f && !stalker) audio_play("slide_0", 0.35f, 0, 1.0f);

    // Te7 footsteps: one per playerStep stepDir flip, counted per input by the simulation
    // (player.c player_step), so the cadence is Krunker's exactly at any frame rate.
    if (me->step_events != track->step_events && !stalker) {
        const float volume = rand_float(0.15f, 0.25f) * (me->crouch_val < 1.0f ? 1.0f : 0.5f);
        play_step(me, 0, volume, me->crouch_val < 1.0f ? 1.0f : 0.8f);
    }

    // HUD health drop: impact_0.
    if (me->health < track->health) audio_play("impact_0", 0.8f, 0, 1.0f);

    // Defuse: planting (the bomb in hand, M1 held on a site) beeps for the planter every 400 ms: interTick -= delta,
    // isYou && interTick <= 0 -> 400, SOUND.play("beep_0", 0.1, false, randFloat(0.95, 1)) (deob-10.0.3.js:192129-192133;
    // interTick starts at 0 each life, :190718). shared/src/defuse.c fills the bar on the same frames.
    if (me->making_progress && me->weapon->bomb && me->team == DEFUSE_ATTACKERS && defuse_site_at(&client->game, me) >= 0) {
        g_sound.plant_tick -= delta;
        if (g_sound.plant_tick <= 0.0f) {
            g_sound.plant_tick = DEFUSE_PLANT_BEEP;
            audio_play("beep_0", 0.1f, 0, rand_float(0.95f, 1.0f));
        }
    }

    track_store(track, me);
}

// ---------------------------------------------------------------- other players

// The track of an interpolated player; *fresh is set on its first sighting (no diff yet).
static PlayerTrack *remote_track(const Player *player, int *fresh) {
    PlayerTrack *free_slot = NULL;
    *fresh = 0;
    for (int i = 0; i < MAX_REMOTE; i++) {
        PlayerTrack *track = &g_sound.remote[i];
        if (track->valid && track->player == player && track->uid == player->uid) return track;
        if (!track->valid && !free_slot) free_slot = track;
    }
    if (free_slot) {
        track_reset(free_slot, player);
        *fresh = 1;
    }
    return free_slot;
}

static void remote_player_events(Client *client, const float delta) {
    for (int i = 0; i < MAX_REMOTE; i++) g_sound.remote[i].seen = 0;

    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *player = client->game.players[i];
        if (!player || player == client->me || !player->active || !player->mesh) continue;

        int fresh;
        PlayerTrack *track = remote_track(player, &fresh);
        if (!track) continue;
        track->seen = 1;
        if (fresh) {
            track_store(track, player);
            continue;
        }

        // Interpolated players (95.js): Te7(distance moved in x/z) while grounded; crouched
        // players' steps advance 1.6x faster but are silent (S = 0 for !MzIM2 when crouched).
        const float moved = distance2(player->position, track->position);
        const int grounded = fabsf(player->velocity.y) < 1e-3f;

        if (grounded && moved > 1e-4f && moved < fmaxf(TELEPORT_DISTANCE, TELEPORT_SPEED * delta) && !player_is_stalker(&client->game, player)) {
            const int crouched = player->crouch_val >= 1.0f;
            if (step_advance(track, moved * (crouched ? 1.6f : 1.0f)) && !crouched &&
                distance3(player->position, client->camera.position) <= STEP_RANGE) {
                play_step(player, 1, rand_float(0.15f, 0.25f), 1.0f);
            }
        }

        track_store(track, player);
    }

    for (int i = 0; i < MAX_REMOTE; i++) {
        if (!g_sound.remote[i].seen) g_sound.remote[i].valid = 0;
    }
}

// ---------------------------------------------------------------- entry points

static void preload_common(void) {
    static const char *COMMON[] = {
        "tick_0", "step_0", "step_1", "step_2", "jump_0", "jump_1", "reload_1", "reload_2", "aim_0", "aim_1",
        "case_0", "case_1", "swsh_0_0", "swsh_0_1", "swsh_def_0", "swsh_def_1", "slide_0", "melee_equip",
        "hit_0", "impact_0", "headshot_0", "bounce_0",
    };
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]); i++) audio_preload(COMMON[i]);
    for (size_t i = 0; i < sizeof(WEAPON_SOUNDS) / sizeof(WEAPON_SOUNDS[0]); i++) audio_preload(WEAPON_SOUNDS[i].sound);
    audio_preload("weapon_29_blast"); // the Charge Rifle's full-charge shot (shoot(..., fullCharge))
    nettspend_audio_preload(); // KrunkNative Nettspend: kn_drank_* / kn_money_throw / kn_laced_ambience
}

void sound_events_tick(Client *client, const float now, const float delta) {
    g_sound.now = now;
    if (!audio_available()) return;

    if (!g_sound.preloaded) {
        g_sound.preloaded = 1;
        g_sound.map_index = -1;
        preload_common();
    }

    map_sounds_update(client);
    nettspend_audio_tick(now); // KrunkNative Nettspend: the laced loop's end

    if (client->game.ready) {
        local_player_events(client, now, delta);
        remote_player_events(client, delta);
    } else {
        g_sound.me.valid = 0;
    }

    // Howler.pos / orientation (deob-10.0.3.js:260850-260851): the head and the controls' look on the input
    // frames, not wherever a class camera sits (class_camera.h); spectating and before a life, the camera
    vec3 ear = client->camera.position;
    float yaw = client->camera.rotation.y, pitch = client->camera.rotation.x;
    class_camera_listener(client, &ear, &yaw, &pitch);
    audio_update(ear, yaw, pitch);
}

void sound_events_hitmarker(void) {
    audio_play("hit_0", 3.1f, 0, 1.0f);
}

void sound_events_kill(const int headshot, const Player *killer, const int weapon_index) {
    // getKillSnd: headshot_0, else the weapon skin's sound (none for default skins).
    // glorp hsSound: every own kill plays headshot_0 when the setting asks for it
    if (headshot || client_settings_hs_sound_always()) {
        audio_play("headshot_0", 1.0f, 0, 1.0f);
        return;
    }
    // getKillSnd (deob-10.0.3.js:198732-198750): wId 12 (the Combat Knife) reads the melee skin, any other weapon
    // player.skins[loadout.indexOf(wId)] (the first slot holding it: skins[0] / skins[1], none past them); a killSnd item
    // with a type (the melees) plays weaponConfig[wId].sound + "_" + id. The server sends networkSounds.indexOf(it) in
    // "6" (:198642) and the killer plays it unless it is 0 or -1 (GET_KILL, :259296-259304).
    const Weapon *weapon = killer && killer->game && weapon_index >= 0 && (size_t) weapon_index < killer->game->weapon_count ? killer->game->weapons[weapon_index] : NULL;
    if (!weapon || !weapon->sound) return;
    int skin_index = -1;
    if (weapon_index == WEAPON_ID_KNIFE) {
        skin_index = killer->skins.melee;
    } else {
        int slot = -1;
        for (int i = 0; killer->loadout && i < killer->loadout_size && slot < 0; i++)
            if (killer->loadout[i] == weapon_index) slot = i;
        skin_index = slot == 0 ? killer->skins.primary : slot == 1 ? killer->skins.secondary : -1;
    }
    char name[64];
    if (!viewer_skin_kill_sound(weapon->sound, viewer_skin(skin_index), name, sizeof(name)) || network_sound_index(name) <= 0) return;
    audio_play(name, 1.0f, 0, 1.0f);
}

void sound_events_remote_shot(Client *client, const Player *shooter, const int blast) {
    // shoot() on the server -> "s" packet -> playerSound -> play3D(weapon.sound, 0.85, rate); a
    // full-charge shot plays weapon.sound + "_blast" (NetShotPacket.first 2), a remodel weapon.sound + "_<mid>"
    // (shot_sound: the shooter's skins and upgrades). The official link's server names it in its own "s" instead.
    if (!shooter || shooter == client->me) return;
    // KrunkNative Nettspend: Money Spread's throw (by weapon id)
    if (shooter->loadout && shooter->loadout_index >= 0 && shooter->loadout_index < shooter->loadout_size &&
        nettspend_audio_remote_shot(shooter->loadout[shooter->loadout_index], shooter->position)) return;
    // KrunkNative Piss (fireMustard on the server): playSound("spray", 0.22, rate 0.85-1.08) at most
    // every 220 ms per sprayer
    if (shooter->weapon && shooter->weapon->stream) {
        int fresh;
        PlayerTrack *track = remote_track(shooter, &fresh);
        const char *stream_sound = shooter->loadout && shooter->loadout_index >= 0 && shooter->loadout_index < shooter->loadout_size
            ? nettspend_audio_stream_sound(shooter->loadout[shooter->loadout_index]) : NULL; // KrunkNative Nettspend: the Drank splashes
        if (!stream_sound) stream_sound = shooter->weapon->sound;
        if (!stream_sound || (track && !fresh && g_sound.now - track->last_spray < PISS_SOUND_INTERVAL)) return;
        if (track) track->last_spray = g_sound.now;
        audio_play_3d(stream_sound, shooter->position, 0.22f, rand_float(0.85f, 1.08f), 0.0f, 0.0f);
        return;
    }
    char name[48];
    if (shot_sound(shooter, blast, name, sizeof(name), NULL) > 0) audio_play_3d(name, shooter->position, 0.85f, rand_float(0.9f, 1.1f), 0.0f, 0.0f);
}

void sound_events_explosion(const vec3 position) {
    audio_play_3d("explosion", position, 1.0f, 1.0f, 0.0f, 0.0f);
}
