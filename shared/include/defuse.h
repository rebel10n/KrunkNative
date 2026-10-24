#pragma once
// Krunker v10.0.3 Defuse (mode.site, MODE_ID_DEF): the bomb flow of shared/src/defuse.c.
//
// The bomb is a weapon (Bomb, id 17: bomb, equipment, type 3) lying in a WEAPON_PICKUP of the map (tm 2: the
// attackers take it with the interact key, players.interact type 6); the sites are the map's BOMB_SITE objects
// (MapManager.addBombSite). An attacker holding the bomb on a site holds M1 on the ground for 2 s (player.update,
// deob-10.0.3.js:192122-192146) and game.plantBomb drops the bomb into its pickup, inactive, where it stands
// (:217725-217747): "Bomb Planted" +200, "BOMB PLANTED", bo_plant, then 40 s of beep_0 (game.updateBomb,
// :217779-217806) before everyone active on the other team dies ("n" 1, players.kill without a killer). A
// defender holding M1 on the ground at the planted bomb for 4 s (:192159-192170) defuses it (game.diffuseBomb,
// :217748-217758): bo_diff and diffuses + 1, which ends the round for the defuser's team (def.condition /
// winCondition, shared/src/modes/mode_rules.c). The bomb counts as a living attacker while planted. A carrier
// killed by someone drops the bomb where it died; a carrier that dies without a killer sends it back to its
// pickup's origin (def.onKill, :119839-119856). The client has no bomb state of its own: it sees the pickup
// ("inat"), the messages, the sounds and the kills, as v10's does.
#include <shared.h>

#define DEFUSE_BOMB_WEAPON 17   // the Bomb: def.onKill's loadout.indexOf(17), plantBomb's dropWeapon(.., 17, ..)
#define DEFUSE_ATTACKERS 2      // team 2 plants (`this.team == 2 && this.weapon.bomb`)
#define DEFUSE_DEFENDERS 1      // team 1 defuses (`this.team == 1`)
#define DEFUSE_PLANT_MS 2000.0f // interProg += delta / 2000
#define DEFUSE_DEFUSE_MS 4000.0f // interProg += delta / 4000
#define DEFUSE_BOMB_TIME 40.0f  // activeBomb.time 40000
#define DEFUSE_BOMB_TICK 2.0f   // activeBomb.tick 2000: the first beep, and the beat until 20 s are left
#define DEFUSE_PLANT_SCORE 200  // "Bomb Planted" [19, 200, 0, 1] and players.score(planter, 200, 2)
#define DEFUSE_PLANT_BEEP 0.4f  // the planter's own beep_0 (isYou): interTick 400 ms

// changeInteract(player, obj, 3, data, 1): GameInteractScan.data of a GAME_INTERACT_BOMB prompt
enum {
    DEFUSE_PROMPT_PLANT = 1,  // "Press [M1] to Plant"
    DEFUSE_PROMPT_EQUIP = 2,  // "Press [equip] to Equip Bomb"
    DEFUSE_PROMPT_DEFUSE = 3, // "Press [M1] to Diffuse"
};

// map.manager.sites.length && mode.site: the bomb flow runs (plantBomb / updateBomb / diffuseBomb's guard)
int defuse_enabled(const Game*);
// The beat of the bomb's beeps with `time_left` seconds on it (updateBomb: 2 s, 1 s from 20 s, 0.5 s from 10 s,
// 0.2 s from 5 s, 0.15 s from 2 s)
float defuse_beep_interval(float time_left);
// player.update's sites and pickups loops for one input frame (both sides: the client predicts the bar,
// the authority plants / defuses): `shoot` held on the ground fills interact_progress (making_progress,
// doing_interact). Called by player_do_map_collisions (not noclip).
void defuse_player_collide(Player*, const Input*, float delta, int recon);
// The uid of the planted (inactive) bomb pickup the player stands at, -1 none (a defender's bar / prompt)
int defuse_bomb_pickup_at(const Game*, const Player*);
// The index of the first BOMB_SITE the player stands in, -1 none
int defuse_site_at(const Game*, const Player*);
// game.plantBomb / game.diffuseBomb (authority)
void defuse_plant(Game*, Player*);
void defuse_defuse(Game*, Player*);
// game.updateBomb (authority, game.update after the vote timers): the beeps and the explosion
void defuse_update(Game*, float delta);
// def.onKill: the victim's bomb drops where it died (a killer) or goes back to its pickup (none)
void defuse_on_kill(Game*, Player *victim, Player *killer);

// shared/src/interact.c: Player.collides(GameObject of `object`, pad) / (a pickup's GameObject, uid)
int game_player_in_object(const Player*, const Object*, float pad);
int game_player_in_pickup(const Game*, const Player*, int uid);
// players.dropWeapon(player, false, weapon, active) (authority, alive or dead): the slot holding `weapon` goes
// back into its empty pickupRep pickup next to the player with `active`; the pickup uid (*at: where it lies),
// -1 dropped without a pickup to go to, -2 nothing dropped.
int game_player_drop_weapon_id(Game*, Player*, int weapon, int active, vec3 *at);
