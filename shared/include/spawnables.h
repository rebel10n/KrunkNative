#pragma once
// WP-D (doc/v7/map-logic.md §4.1c, §7 WP-D): v7.1.4 spawnables (data/medals.js: 25 types) and the
// mode hooks other packages call. Bodies in shared/src/spawnables.c (WP-D).
#include <shared.h>

struct GameAI_t;

// medals.js indices ("crsp" type)
typedef enum {
    SPAWNABLE_TAG = 0,          // Kill Confirmed tag (8 s)
    SPAWNABLE_GIFT,             // 1 (account skins: no effect without kn_brain)
    SPAWNABLE_GOLDEN_EGG,       // 2
    SPAWNABLE_KR_TAG,           // 3 Deposit KR Tag (12 s, data.c)
    SPAWNABLE_DIAMOND_EGG,      // 4
    SPAWNABLE_SLIME_PAD,        // 5 (alwaysActive; Slimer deployable)
    SPAWNABLE_AMMO_PACK,        // 6 All or Nothing
    SPAWNABLE_WEAPON_BOX,       // 7 Mag Dump
    SPAWNABLE_MELEE_PICKUP,     // 8
    SPAWNABLE_AMMO_CACHE,       // 9 Zombies power-ups 9..15
    SPAWNABLE_NUKE,             // 10
    SPAWNABLE_MINIGUN,          // 11
    SPAWNABLE_INSTA_KILL,       // 12
    SPAWNABLE_DISCOUNT,         // 13
    SPAWNABLE_DOUBLE_POINTS,    // 14
    SPAWNABLE_GRENADE_LAUNCHER, // 15
    SPAWNABLE_JOE_TAG,          // 16
    SPAWNABLE_FROG,             // 17
    SPAWNABLE_KRUNKANIUM,       // 18 (Hardpoint pub kill drop)
    SPAWNABLE_PERK_BOTTLE,      // 19
    SPAWNABLE_ZOMBIE_PORTAL,    // 20 (aiOnly)
    SPAWNABLE_SKULL_EE,         // 21 (canShoot)
    SPAWNABLE_FREE_UPGRADE,     // 22
    SPAWNABLE_MEGA_SLIME_PAD,   // 23
    SPAWNABLE_LOOT_DROP,        // 24 (playerSpecific)
    SPAWNABLE_COUNT
} SpawnableType;

// medals.js entry (the parts the server and the renderer need)
typedef struct {
    const char *name;
    const char *model;          // models/<model>.obj (client)
    float scale;
    float pad_mlt;              // padMlt (0 = 1)
    float y_off;
    float timer;                // seconds (0: never despawns)
    unsigned char ai_only:1;
    unsigned char player_specific:1;
    unsigned char always_active:1;
    unsigned char can_shoot:1;
    unsigned char no_anim:1;
    unsigned char powerup:1;
} SpawnableInfo;

const SpawnableInfo *spawnable_info(int type); // NULL for an unknown type

// medals.js data ({c, wId, xD, zD}); unset fields: c 0, w -1, xd / zd 0
typedef struct {
    int c;
    int w;
    float xd, zd;
} SpawnableData;

// game.js spawnable drop ("crsp" type, sid, x, y, z, team, data): KR tags, ammo packs, weapon
// boxes, melee pickups, Krunkanium... `data` is data.c. Returns the spawnable sid, -1 when nothing
// spawned. position.y gets the type's yOff added, as game.dropSpawnable does.
int spawnable_drop(Game*, int type, Player *owner, int data, vec3 position);
int spawnable_drop_ex(Game*, int type, Player *owner, const SpawnableData*, vec3 position);
void spawnables_update(Game*, float delta);      // game.update #19: pickups, timers ("dsp")
void spawnables_reset(Game*);                    // finishInit resetSpawnables
void spawnables_send_all(Game*, int uid);        // sync.spwns for a joiner
int spawnable_alive(const Game*, int sid);       // KrunkNative Object Manager: an active spawnable has this sid
int spawnable_remove(Game*, int sid);            // KrunkNative Object Manager: despawn it now ("dsp"); 1 when it was active
int spawnable_shoot(Game*, Player*, int sid);    // game.shootSpawnable (canShoot); 1 when consumed
// game.pickupSpawnable(ai, s) for aiOnly spawnables (the Zombie Portal), called by WP-F's AI
// collision pass once per active AI. 1 when it consumed one.
int spawnable_pickup_ai(Game*, struct GameAI_t *ai);
#define SPAWNABLES_HAVE_AI_HOOKS 1 // spawnable_pickup_ai + mode_v7_on_ai_death exist (WP-F calls them)

// mode.collisionCheck(player, obj) (king onZone, dom occupancy): called by the WP-B collision
// chain for every object the player touches. Returns 1 when the mode consumed the contact.
// The v7 modes test their own zones (mode_tick), so this only reports.
int mode_v7_collision_check(Game*, Player*, const Object*);
// trigger action 71 "Mode Specific" (zombies portal ...) and event 27 onObjectiveSwitch.
void mode_v7_on_trigger(Game*, Player*, int value);
// mode.onAIDeath (AI.kill after the onAIDeath trigger; authority): WP-F calls it.
void mode_v7_on_ai_death(Game*, struct GameAI_t *ai, Player *killer);
