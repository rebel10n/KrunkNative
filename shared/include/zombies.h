#pragma once
// map-logic WP-H stage 2 (doc/v7/map-logic.md §2.7, §4.1b, §4.4): the Krunker v7.1.4 zombies
// machines (mystery box, perk machine, krum machine), power-ups (insta-kill, fire sale, double
// points), finiteAmmo reserves, the Zombies waves (shared/src/modes/mode_zombies.c) and airdrops
// (shared/src/airdrop.c). State lives in Game.zombies (shared.h GameZombies); the machines are
// map-placed and work in any mode, as in v7.
//
// Sources: v7 src/data/game.js updatePowerups / mystery box / krum machine (:1409-1450,
// :2194-2588), players.js interact types 13-15 (:3367-3433) and givePerk / giveWeapon, player.js
// changeInteract (:1352-1389) and the airdrop claim (:1718-1728), data/airDrop.js,
// airDropPackages.js, streaks.js Airdrop, config.js :937-945.
#include <shared.h>

struct GameAI_t;

// ---- packets: NET_PACKET_BLOCK_ZOMBIES (150-159) and NET_PACKET_BLOCK_AIRDROP (177-179). The
// server sends them only to peers whose HELLO announced NET_PROTOCOL_MINOR_V7 (server/src/net.c);
// clients apply them with zombies_apply_packet (client/src/net/zombie_events.c).
#define NET_PACKET_ZOMBIE_MACHINE (NET_PACKET_BLOCK_ZOMBIES + 0)  // S->C all: NetZombieMachinePacket ("mybx" / "krm")
#define NET_PACKET_MYSTERY_BOX_SWITCH (NET_PACKET_BLOCK_ZOMBIES + 1) // S->C all: NetMysteryBoxSwitchPacket ("smybx uid, enable")
#define NET_PACKET_POWERUP (NET_PACKET_BLOCK_ZOMBIES + 2)         // S->C all: NetPowerupPacket ("pwup index, ms")
#define NET_PACKET_ZOMBIE_WAVE (NET_PACKET_BLOCK_ZOMBIES + 3)     // S->C all: NetZombieWavePacket ("upz wave, specialName")
#define NET_PACKET_RESERVE_AMMO (NET_PACKET_BLOCK_ZOMBIES + 4)    // S->C to the player: NetReserveAmmoPacket ("uamo" reserves)
#define NET_PACKET_SHARING (NET_PACKET_BLOCK_ZOMBIES + 5)         // S->C to the player: NetSharingPacket ("sic")
#define NET_PACKET_ZOMBIE_STATS (NET_PACKET_BLOCK_ZOMBIES + 6)    // S->C all: NetZombieStatsPacket (endData aiKills / rounds)
#define NET_PACKET_AIRDROP (NET_PACKET_BLOCK_AIRDROP + 0)         // S->C per viewer: NetAirDropPacket ("ad")
#define NET_PACKET_AIRDROP_REMOVE (NET_PACKET_BLOCK_AIRDROP + 1)  // S->C all: NetAirDropRemovePacket ("rad")
#define NET_PACKET_AIRDROP_CLAIM (NET_PACKET_BLOCK_AIRDROP + 2)   // S->C all: NetAirDropClaimPacket ("krp owner, claimer")

typedef enum {
    ZOMBIE_MACHINE_MYSTERY_BOX = 0,
    ZOMBIE_MACHINE_KRUM = 1,
} ZombieMachineKind;

typedef struct {
    uint8_t kind;               // ZombieMachineKind
    uint8_t active;
    uint8_t state;
    int32_t uid;
    float timer;                // ms
    int32_t pending_weapon;     // weapon id, -1 teddy, ZOMBIES_NONE
    int32_t purchased_by;       // uid, -1 anyone, ZOMBIES_NONE
} NetZombieMachinePacket;

typedef struct {
    int32_t uid;                // the box to activate
    uint8_t enable;             // fire sale: every box on
} NetMysteryBoxSwitchPacket;

typedef enum {
    ZOMBIES_POWERUP_INSTA_KILL = 0,
    ZOMBIES_POWERUP_FIRE_SALE = 1,
    ZOMBIES_POWERUP_DOUBLE_POINTS = 2,
} ZombiesPowerup;

typedef struct {
    uint8_t index;              // ZombiesPowerup
    int32_t ms;                 // time left, 0 = over
} NetPowerupPacket;

typedef struct {
    int32_t wave;
    char special[24];           // FRENZY / ONSLAUGHT / ... ("" an ordinary wave)
} NetZombieWavePacket;

typedef struct {
    uint8_t count;
    int32_t weapons[PLAYER_RESERVE_MAX];
    int32_t ammo[PLAYER_RESERVE_MAX];
} NetReserveAmmoPacket;

typedef struct {
    uint8_t on;
} NetSharingPacket;

// zom.js endStats "zombies" (endForm: aiKills) and "rounds" (player.rounds) of every player,
// sent with the end of a Zombies game, before NET_PACKET_MODE_END (whose stats array has no room
// left under NET_MAX_PAYLOAD). "downs" is the deaths every client already has.
typedef struct {
    int32_t uid;
    int16_t zombies;            // aiKills
    int16_t rounds;
} NetZombieStatsEntry;

typedef struct {
    uint8_t count;
    NetZombieStatsEntry entries[NET_MODE_END_MAX];
} NetZombieStatsPacket;
_Static_assert(sizeof(NetZombieStatsPacket) <= NET_MAX_PAYLOAD, "NetZombieStatsPacket exceeds NET_MAX_PAYLOAD");

typedef struct {
    int32_t sid;
    float fall;                 // fallDist left
    float position[3];          // x, y (current), z
    uint8_t index;              // airDropPackages index this viewer sees (imitate for non-owners)
    int32_t owner_uid;
} NetAirDropPacket;

typedef struct {
    int32_t sid;
} NetAirDropRemovePacket;

typedef struct {
    int32_t owner_uid;
    int32_t claimer_uid;        // == owner unless stolen ("thief")
} NetAirDropClaimPacket;

// config.js
#define ZOMBIES_MYSTERY_BOX_ANIM 5000.0f          // mysteryBoxAnim (ms)
#define ZOMBIES_MYSTERY_BOX_DESPAWN 15000.0f      // mysteryBoxDespawn
#define ZOMBIES_MYSTERY_BOX_DESPAWN_TEDDY 8000.0f // mysteryBoxDespawnTeddy
#define ZOMBIES_KRUM_ANIM 5000.0f                 // krumMachineAnim
#define ZOMBIES_KRUM_DESPAWN 15000.0f             // krumMachineDespawn
#define ZOMBIES_MACHINE_RADIUS 7.0f               // tRadius of every machine
#define ZOMBIES_PERK_COUNT 7                      // config.zombiePerks.length
#define ZOMBIES_FIRE_SALE_PRICE 10
#define ZOMBIES_TEDDY_CHANCE 20                   // getMysteryBoxWeapon(20) (0 for sharingIsCaring)
extern const char *const g_zombie_perk_names[ZOMBIES_PERK_COUNT]; // config.zombiePerks

// ---- lifecycle (shared/src/zombies.c)
void zombies_round_start(Game*);           // finishInit: machines from the map, power-ups off, airdrops gone, switchMysteryBox (authority)
void zombies_tick(Game*, float delta);     // seconds: updatePowerups (#8), airDrop.update / sync (#14), mystery boxes / krum machines (#21-22)
void zombies_send_state(Game*, int uid);   // init syncData pwup / mybx / krm, the wave and the airdrops for a joiner
void zombies_send_end_stats(Game*);        // NET_PACKET_ZOMBIE_STATS of every player (authority, game end)
// Client mirror of every WP-H packet; `me` (the local player, may be NULL) takes the reserves /
// sharing ones. 1 = a WP-H packet.
int zombies_apply_packet(Game*, Player *me, int type, const void *payload, uint16_t length);

// Zombie Portal onDestroy (taken or timed out): the easter egg may open it again
// (eePAPActive off, globalVals.portal = 0) until it is completed. shared/src/modes/mode_zombies.c.
void mode_zombies_portal_closed(Game*);

// ---- power-ups
int zombies_insta_kill(const Game*);       // instaKillTimer: a player's hit kills any AI
int zombies_double_points(const Game*);    // doublePointsTimer: positive score x2
void zombies_set_powerup(Game*, int index, float ms); // timer + "pwup" (fire sale switches the boxes)

// ---- machines
int zombies_mystery_box_price(const Game*, const Player*, const Object *box); // fireSale 10, sharingIsCaring ceil(sp / 2), else sp
int zombies_active_mystery_box(const Game*);                  // getActiveMysteryBox: object uid, -1 none
void zombies_switch_mystery_box(Game*, int uid, int enable);  // switchMysteryBox(uid, enable); uid < 0 picks one
int zombies_mystery_box_weapon(const Game*, int teddy_chance); // getMysteryBoxWeapon: weapon id, -1 teddy / none
const ZombieMachine *zombies_machine(const Game*, int uid);   // the box / krum machine of an object, NULL none
// changeInteract for one map object (GAME_INTERACT_MYSTERY_BOX / PERK / KRUM), GAME_INTERACT_NONE
// when it offers nothing (game_interact_scan)
int zombies_interact_prompt(const Game*, const Player*, const Object*, int uid);
// players.interact types 13 / 14 / 15 on map object `uid` (game_player_interact_v7, authority)
void zombies_player_interact(Game*, Player*, int type, int uid);
void zombies_perk_machine_touch(Game*, Player*, int uid);     // player.js gotFreeP: +50 for crouching on one
void zombies_melee_object(Game*, Player*, const Object*);     // a melee hit on your ready box shares it
// a hitscan ray (origin, dir: the full range) shoots the first canShoot spawnable before stop_t
void zombies_shoot_spawnables(Game*, Player*, vec3 origin, vec3 dir, float stop_t);
// giveWeapon(player, id, upgraded, true): the zombies slot rules (weaponLimit 2, perk 6: 3)
void zombies_give_weapon(Player*, int weapon_id, int upgraded);

// ---- finiteAmmo reserves (Zombies; bots have none)
int zombies_finite_ammo(const Player*);
int player_reserve_ammo(Player*, int slot);         // the slot's reserve (created at magReserve full mags)
void player_set_reserve_ammo(Player*, int slot, int value);
void zombies_reserves_reset(Player*);               // spawn / resetAmmo: every weapon at full reserve
void zombies_send_reserves(Player*);                // "uamo" reserves (authority)
int zombies_reload_allowed(Player*);                // 0: finiteAmmo and the reserve is empty
void zombies_reload_finish(Player*);                // the reload refill from the reserve (finiteAmmo)

// ---- airdrops (shared/src/airdrop.c). Public lobbies only (doc/v7/decisions.md): never offline
// or in a hosted / sandbox game.
#define AIRDROP_PACKAGE_COUNT 22
#define AIRDROP_FALL_SPEED 0.05f    // units per ms
#define AIRDROP_CLAIM_RADIUS 6.0f
#define AIRDROP_HEIGHT 500.0f       // the drop starts this far above the caller
typedef struct {
    const char *name;
    int rarity;                     // vA764 rarity index
    int icon;
    float claim_time;               // ms
    int streak;                     // the streak it calls in, -1 none
    unsigned char imitate:1;        // trapped
    unsigned char animate:1;
} AirDropPackage;
extern const AirDropPackage g_airdrop_packages[AIRDROP_PACKAGE_COUNT];
extern const char *const g_airdrop_rarity_colors[7];
int airdrop_allowed(const Game*);   // a public lobby: not offline, not hosted / sandbox
int airdrop_streak_activate(Game*, Player*); // streaks.js Airdrop.activate: 1 when it dropped
// airDrop.drop: a drop at `position` (the top of its fall) for `owner`; fall < 0 computes the
// fall distance, package >= 0 forces the package. Returns the sid, -1 none.
int airdrop_drop(Game*, Player *owner, vec3 position, float fall, int package);
int airdrop_pick_package(const Game*, int level, int *imitate); // tier by account level (seeded RNG)
void airdrop_tick(Game*, float delta_ms);          // update + sync (authority), the fall (clients)
void airdrop_player_collide(Player*, const Input*, float delta); // the claim hold (client prediction too)
int airdrop_interact_prompt(const Game*, const Player*); // the drop slot a player can claim, -1 none
void airdrop_send_all(Game*, int uid);
int airdrop_apply_packet(Game*, int type, const void *payload, uint16_t length); // client: "ad" / "rad" / "krp"
void airdrop_reset(Game*);
