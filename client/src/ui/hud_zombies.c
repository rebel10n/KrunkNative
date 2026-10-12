// The Krunker v7.1.4 zombies HUD on the v7 page (map-logic WP-H, client side), from the
// Game.zombies mirror (shared/include/zombies.h). One per-frame call, hud_zombies_tick(), after
// client_hud_html_tick():
//
//   #powerUpHolder   io_updatePowerUp: the .powerupItem of power-up i (insta-kill, fire sale,
//                    double points) is shown while its timer runs; game.updatePowerups writes
//                    its .powerupTime as UTILS.getChargedTime(ms) ("SS:t", seconds mod 60).
//   #ammoMax         players.updatePlayerAmmo with mode.finiteAmmo (not bots): "| <reserve>"
//                    ("| -" for none, or under mode.ammoLimit), and #reloadMsg only while the
//                    reserve can refill the empty magazine. hud_ui.c writes the plain weapon
//                    form; this pass runs after it in the same frame and rewrites the text.
//   #roundMessage    io_updateZombieRound ("upz"): "Round <wave>" (NET_PACKET_ZOMBIE_WAVE);
//                    emptied when a new game resets the wave.
//
// The zombies end table columns (zombies, downs, rounds) are hud_modes.c's endStats values
// (NET_PACKET_ZOMBIE_STATS for aiKills / rounds).
//   #zPerksHolder    io_updateZombiePerks (NET_PACKET_PERKS, own player): one .zPerkItem per
//                    perk with background img/perks/zombie/<perk>.png. Those icons are not in
//                    the v7.1.4 tree; they come from Krunker's live asset host
//                    (tools/ui-import/fetch_live_assets.py, ui/img).
#include <ui_logic.h>
#include <html_dom.h>
#include <html_ui.h>
#include <zombies.h>
#include <stdio.h>
#include <string.h>

static int g_hz_wave = -1; // the wave #roundMessage was written for, -1 none
static char g_hz_perks[64] = "-"; // the perk list #zPerksHolder shows ("-" not written yet)

// writes only what differs from the page (a page reload or hud_ui.c's own writes are picked up)
static void hz_set_display(const HtmlElem el, const char *value) {
    char now[16];
    const int set = html_dom_get_style(el, "display", now, sizeof(now)) >= 0;
    if (set ? !strcmp(now, value) : !value[0]) return;
    html_dom_set_display(el, value);
}

static void hz_set_text(const HtmlElem el, const char *text) {
    char now[64];
    if (html_dom_get_text(el, now, sizeof(now)) >= 0 && !strcmp(now, text)) return;
    html_dom_set_text(el, text);
}

// UTILS.getChargedTime
static void hz_charged_time(const float ms, char *out, const size_t size) {
    const int t = ms > 0.0f ? (int) ms : 0;
    snprintf(out, size, "%02d:%d", t / 1000 % 60, t % 1000 / 100);
}

static void hz_powerups(const Game *game) {
    const HtmlElem holder = html_dom_by_id("powerUpHolder");
    if (!holder) return;
    HtmlElem items[ZOMBIES_POWERUPS];
    const int count = html_dom_query_all(holder, ".powerupItem", items, ZOMBIES_POWERUPS);
    for (int i = 0; i < count && i < ZOMBIES_POWERUPS; i++) {
        const float ms = game->zombies.powerups[i];
        hz_set_display(items[i], ms > 0.0f ? "" : "none");
        if (ms <= 0.0f) continue;
        char text[16];
        hz_charged_time(ms, text, sizeof(text));
        const HtmlElem time = html_dom_query(items[i], ".powerupTime");
        if (time) hz_set_text(time, text);
    }
}

// the reserve of the held slot (reserveAmmos[loadoutIndex]), -1 none yet; read-only
static int hz_reserve(const Player *me) {
    if (!me->loadout || me->loadout_index < 0 || me->loadout_index >= me->loadout_size) return -1;
    const int id = me->loadout[me->loadout_index];
    for (int i = 0; i < me->reserve_count; i++) {
        if (me->reserve_weapons[i] == id) return me->reserve_ammo[i];
    }
    return -1;
}

static void hz_ammo(const Client *client) {
    const Player *me = client->me;
    if (!client->in_game || !me || !me->active || !zombies_finite_ammo(me) || !me->weapon) return;
    const GameMode *mode = client->game.mode;
    const int ammo_limit = mode && mode->config.ammo_limit > 0;
    const int reserve = hz_reserve(me);
    char text[32];
    if (ammo_limit || reserve <= 0) snprintf(text, sizeof(text), "| -");
    else snprintf(text, sizeof(text), "| %d", reserve);
    const HtmlElem max = html_dom_by_id("ammoMax");
    if (max) hz_set_text(max, text);

    // reloadMsg: the weapon has a magazine, it is empty, no ammoLimit, and the reserve can refill it
    const int slot = me->loadout_index;
    const int empty = slot >= 0 && slot < me->loadout_size && me->ammo[slot] <= 0;
    const int need = me->weapon->ammo && !me->weapon->melee && empty && !ammo_limit && reserve > 0;
    const HtmlElem reload = html_dom_by_id("reloadMsg");
    if (reload) hz_set_display(reload, need ? "block" : "none");
}

static void hz_round(const Game *game) {
    const int wave = game->zombies.wave;
    if (wave <= 0 && g_hz_wave <= 0) return; // no wave yet: the element is not ours
    const HtmlElem el = html_dom_by_id("roundMessage");
    if (!el) return;
    char text[32];
    if (wave > 0) snprintf(text, sizeof(text), "Round %d", wave);
    else text[0] = 0;
    hz_set_text(el, text);
    g_hz_wave = wave;
}

// io_updateZombiePerks: rewritten when the own player's perks change
static void hz_perks(const Client *client) {
    const Player *me = client->me;
    char key[sizeof(g_hz_perks)];
    size_t len = 0;
    key[0] = 0;
    const int count = me ? me->perk_count : 0;
    for (int i = 0; i < count && len + 8 < sizeof(key); i++) len += (size_t) snprintf(key + len, sizeof(key) - len, "%d,", me->perks[i]);
    if (!strcmp(key, g_hz_perks)) return;
    const HtmlElem holder = html_dom_by_id("zPerksHolder");
    if (!holder) return;
    char html[PLAYER_PERK_MAX * 160 + 1];
    size_t n = 0;
    html[0] = 0;
    for (int i = 0; i < count && n + 160 < sizeof(html); i++)
        n += (size_t) snprintf(html + n, sizeof(html) - n,
                               "<div class='zPerkItem' style='background-image: url(&quot;img/perks/zombie/%d.png&quot;)'><span></span></div>",
                               me->perks[i]);
    html_dom_set_html(holder, html);
    snprintf(g_hz_perks, sizeof(g_hz_perks), "%s", key);
}

void hud_zombies_tick(Client *client) {
    if (!client || !html_ui_available() || !html_ui_is_ready() || !client->game.ready) return;
    hz_powerups(&client->game);
    hz_ammo(client);
    hz_round(&client->game);
    hz_perks(client);
}
