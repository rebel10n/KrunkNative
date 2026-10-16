#pragma once
// Krunker Strike's economy on the client (client/src/ui/strike_econ.c): the signed-in account's
// Strike wallet (krunkies, spin tickets, golden tickets), XP / level, inventory, loadout, track
// claims and classes, kept in sync with kn_brain's /strike gateway (services/brain/strike_econ.c)
// over the social socket's gapi tunnel; and the requests the Strike screens make (spins, store,
// equip, progression, missions, leaderboards). Everything is inert unless flavor_get() is
// FLAVOR_STRIKE. The static catalogue (items, rarities, wheels, products, missions, track, the XP
// curve) is shared/include/strike_economy.h (strike_economy()).
//
// For the Store / Spin / Customize / Leaderboard / Missions / Rewards / Progression screens:
//   - read state with strike_econ_account() / strike_econ_owns() / strike_econ_equipped() / ...
//   - start requests with strike_econ_spin() / _buy() / _equip() / ...: the callback gets the gapi
//     status (200 ok, 4xx refusal with error {code, message}, 0 no brain) and the FRVR-shaped data;
//     on success the account state is already updated when it runs
//   - re-render on strike_econ_listen() events (STRIKE_ECON_CHANGED after every state change)
//   - page markup can call the knStrike* handlers directly (strike_econ_handle)
// Equipped skins are worn in game: ui_loadout_look() takes Strike's loadout under FLAVOR_STRIKE
// (strike_econ_apply_look), so the ENT, the class preview and the HUD show them; a Strike game
// server checks them against the account's Strike inventory (kn_brain answers its auth with it).
#include <stdint.h>
#include <cJSON.h>
#include <html_dom.h>
#include <shared.h>          // PlayerSkins, PlayerCosmetics, PlayerCosmeticsV7
#include <strike_economy.h>  // the catalogue, StrikeCurrency, StrikeLevel

typedef struct {
    int account_id;               // 0: no Strike account state (guest / not loaded)
    int loaded;                   // /players/me arrived for account_id
    int64_t wallet[STRIKE_CUR_COUNT]; // wallet[STRIKE_CUR_KR] = krunkies ... (wallet[STRIKE_CUR_XP] = xp)
    int64_t xp;
    StrikeLevel level;            // the bundle's curve (strike_level_for_xp)
    int max_level;                // the track's last level (maxLevelCap)
    int golden_spin_count;        // the golden spin meter, out of spins_to_golden
    int spins_to_golden;
    int missions_unlocked;
    int64_t kills, deaths, wins, games, score, time_ms; // Strike stats
} StrikeAccount;

typedef enum {
    STRIKE_ECON_CHANGED = 0,      // wallet / items / loadout / progress changed (re-render)
    STRIKE_ECON_SIGNED_OUT,       // the state was cleared
} StrikeEconEvent;

typedef void (*StrikeEconFn)(void *user, int status, const cJSON *data, const cJSON *error);
typedef void (*StrikeEconListener)(void *user, StrikeEconEvent event);

// ---- state ----------------------------------------------------------------------------------
const StrikeAccount *strike_econ_account(void);   // never NULL; account_id 0 = none
int strike_econ_item_count(const char *item_id);  // owned copies (0 = not owned; default items: 1)
int strike_econ_owns(const char *item_id);
const cJSON *strike_econ_items(void);             // /players/me/items: [{_id, itemId, count, item}] (NULL before load)
const char *strike_econ_equipped(const char *slot); // the item in a loadout slot ("w1", "t1", ...), NULL = default
int strike_econ_slot_skin(const char *slot);      // the store.skins index the slot wears, -1 = the plain look
int strike_econ_class_owned(int class_id);        // starter classes + class items
int strike_econ_track_claimed(const char *slot_id);
// The track slots the account may claim now (level reached, not claimed): writes up to `max` slot
// ids (pointers into the catalogue), returns the count.
int strike_econ_track_claimable(const char **out, int max);

// ---- requests (0 = not sent: no brain / not Strike / not signed in; fn not called) -------------
int strike_econ_refresh(StrikeEconFn fn, void *user);                  // GET players/me + items
int strike_econ_spin(const char *wheel_id, int count, int golden, StrikeEconFn fn, void *user); // POST v2/spin-wheels/:id/spin
int strike_econ_buy(const char *product_id, int quantity, StrikeEconFn fn, void *user);       // POST v1/shop/purchase
int strike_econ_equip(const char *item_id, StrikeEconFn fn, void *user);                     // POST players/me/equip
int strike_econ_unequip(const char *slot, StrikeEconFn fn, void *user);
int strike_econ_claim_level(const char *slot_id, int choice, StrikeEconFn fn, void *user);   // POST players/me/claim/progression
int strike_econ_claim_mission(const char *mission_id, StrikeEconFn fn, void *user);           // POST missions/claim/:id
// read-only lists, passed through: GET shop / missions/me / v2/spin-wheels/list (details: POST
// v2/spin-wheels/details with the pools' items) / game-config / leaderboards/:board
int strike_econ_get_shop(StrikeEconFn fn, void *user);
int strike_econ_get_missions(StrikeEconFn fn, void *user);
int strike_econ_get_wheels(int with_items, StrikeEconFn fn, void *user);
int strike_econ_get_game_config(StrikeEconFn fn, void *user);
int strike_econ_get_leaderboard(const char *board, int limit, int offset, StrikeEconFn fn, void *user); // xp|kills|wins|score|games
// Any /strike gateway path ("/v1/...", "/v2/..."): the escape hatch for what the helpers lack.
int strike_econ_request(const char *method, const char *path, const cJSON *body, StrikeEconFn fn, void *user);
void strike_econ_cancel(StrikeEconFn fn, void *user); // forget pending callbacks of (fn, user)

int strike_econ_listen(StrikeEconListener fn, void *user);   // up to 8; 0 when full
void strike_econ_unlisten(StrikeEconListener fn, void *user);

// ---- engine hooks ---------------------------------------------------------------------------
void strike_econ_tick(void); // per frame (backend_startup.c): sign-in / sign-out follow active_account()
// ui_loadout_look() under FLAVOR_STRIKE: what the player wears is Strike's loadout only (Box Office
// picks never leak in): weapon skins of the primary / secondary weapon ids, melee, hat, back, dye,
// waist, face, shoe, pet, wrist, charm. Returns 1 when applied (FLAVOR_STRIKE), else 0 (untouched).
int strike_econ_apply_look(int primary_weapon, int secondary_weapon, PlayerSkins *skins, PlayerCosmetics *cosmetics,
                           PlayerCosmeticsV7 *v7);
int strike_econ_spray(void); // the equipped spray's store index under FLAVOR_STRIKE, -1 none / not Strike
// Page handlers (ui_handlers.c MODULE_HANDLERS): knStrikeRefresh(), knStrikeEquip(itemId),
// knStrikeUnequip(slot), knStrikeSpin(wheelId, count, golden), knStrikeBuy(productId, quantity),
// knStrikeClaimLevel(slotId, choice), knStrikeClaimMission(missionId). -1 = not ours.
int strike_econ_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc);
