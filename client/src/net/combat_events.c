// WP-B: the client side of the v7.1.4 combat packets (shared.h "WP-B combat packets"): status
// effects ("pk" -> io_effect), kill streaks ("st" -> io_killStreak, "k<slot>" callInStreak),
// the Nuke ("n" -> io_nukeUpdate), Juggernaut ("jg") and perks ("upk"), and the streak loadout
// sent before the ENT (v7 ENT streakIndexes). The HUD (killStreakHolder, perkHolder,
// zPerksHolder) reads Player.streaks_owned / effects / perks; the server owns every decision.
#include <client.h>
#include <audio.h>
#include <spectate.h>
#include <ui_logic.h>
#include <html_dom.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMBAT_READ(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

// The protocol the server announced in INIT (main.c): the new C->S packets only go to v7 servers.
static int combat_server_v7(const Client *client) {
    return client->offline || client->net_server_minor >= NET_PROTOCOL_MINOR_V7;
}

// getSavedVal(key) (the page's localStorage: ui_loadout.c's picks), `fallback` when unset.
static int setting_int(const char *key, const int fallback) {
    char buf[32];
    if (html_storage_get(key, buf, sizeof(buf)) < 0 || !buf[0]) return fallback;
    char *end;
    const long v = strtol(buf, &end, 10);
    return end != buf ? (int) v : fallback;
}

// index.js streakIndex1..3 (getSavedVal, f1795 keeps a pick only in its own slot; the server
// sanitises again). Unset keys: slot 2 = Nuke, the others none.
void combat_send_streak_slots(Client *client) {
    if (!combat_server_v7(client)) return;
    static const char *const keys[STREAK_SLOTS] = {"streakIndex1", "streakIndex2", "streakIndex3"};
    NetStreakSlotsPacket packet;
    for (int i = 0; i < STREAK_SLOTS; i++) {
        const int s = setting_int(keys[i], g_default_streak_indexes[i]);
        packet.indexes[i] = (int8_t) (s >= 0 && s < STREAK_COUNT && g_streaks[s].slot == i ? s : -1);
    }
    client_net_send(client, NET_PACKET_STREAK_SLOTS, &packet, sizeof(packet));
}

// window.callInStreak(slot): io.send("k" + slot)
void combat_call_in_streak(Client *client, const int slot) {
    if (slot < 0 || slot >= STREAK_SLOTS || !combat_server_v7(client)) return;
    const NetUseStreakPacket packet = {.slot = (uint8_t) slot};
    client_net_send(client, NET_PACKET_USE_STREAK, &packet, sizeof(packet));
}

const char *combat_scope_overlay_src(char *buf, const size_t n) {
    const int index = setting_int("scopeIndex", 0);
    if (index == -1) return NULL;
    if (index == -2) {
        char custom[512];
        if (html_storage_get("savedScope", custom, sizeof(custom)) >= 0 &&
            (!strncmp(custom, "https://", 8) || !strncmp(custom, "http://", 7)) && !strstr(custom, ".svg")) {
            snprintf(buf, n, "%s", custom);
            return buf;
        }
    }
    const int id = index >= 0 && index < SCOPE_OVERLAY_COUNT ? g_scope_overlays[index].id : 0;
    snprintf(buf, n, "textures/scopes/scope_%d.png", id);
    return buf;
}

int player_attach_index(const Player *player) {
    if (!player) return DEFAULT_ATTACHMENT_INDEX;
    if (player->is_you) return setting_int("attachIndex", DEFAULT_ATTACHMENT_INDEX); // getSavedVal("attachIndex") || 0
    return player->attach_index;
}

int combat_events_handle(Client *client, const int type, const void *payload, const uint16_t length) {
    switch (type) {
        case NET_PACKET_EFFECT: { // io_effect(index, count): the local player's stacks
            COMBAT_READ(NetEffectPacket, effect);
            if (client->me && effect.effect < EFFECT_COUNT) player_set_effect_stacks(client->me, effect.effect, effect.count);
            return 1;
        }
        case NET_PACKET_STREAK: { // io_killStreak(index, count)
            COMBAT_READ(NetStreakPacket, streak);
            if (client->me && streak.streak < STREAK_COUNT) client->me->streaks_owned[streak.streak] = streak.count;
            return 1;
        }
        case NET_PACKET_NUKE: { // io_nukeUpdate: siren on call-in, the blast on detonation
            COMBAT_READ(NetNukePacket, nuke);
            audio_play(nuke.state ? "nuke_0" : "siren_0", 1.0f, 0, 1.0f);
            return 1;
        }
        case NET_PACKET_JUGGERNAUT: { // io_setJuggernaut (+ addPlayerSound jug_0 loop, maxHealth)
            COMBAT_READ(NetJuggernautPacket, jug);
            Player *player = client_find_player(client, jug.uid);
            if (player) {
                player->is_juggernaut = jug.on;
                if (jug.max_health > 0) player->max_health = jug.max_health;
            }
            if (jug.on) audio_play("jug_0", 0.5f, 1, 1.0f);
            else audio_stop("jug_0");
            return 1;
        }
        case NET_PACKET_WEAPON_STREAK: { // io_updateWeaponStreak(weapon, count): the weapon card's streak
            COMBAT_READ(NetWeaponStreakPacket, wstk);
            if (client->me) {
                client->me->streak_weapon = wstk.weapon;
                client->me->weapon_streak = wstk.count;
            }
            return 1;
        }
        case NET_PACKET_UPGRADES: { // "inat" upgrades[]: krumMachine slots and their ammo
            COMBAT_READ(NetUpgradesPacket, up);
            Player *player = client_find_player(client, up.uid);
            if (!player) return 1;
            player_set_upgrades(player, up.slots);
            for (int i = 0; i < player->loadout_size && i < NET_LOADOUT_MAX; i++) {
                if (player->ammo) player->ammo[i] = (unsigned int) up.ammo[i];
            }
            return 1;
        }
        case NET_PACKET_PERKS: { // io_updateZombiePerks(sid, perks)
            COMBAT_READ(NetPerksPacket, perks);
            Player *player = client_find_player(client, perks.uid);
            if (!player) return 1;
            player->perk_count = perks.count < PLAYER_PERK_MAX ? perks.count : PLAYER_PERK_MAX;
            for (int i = 0; i < player->perk_count; i++) player->perks[i] = perks.perks[i];
            if (perks.max_health > 0) player->max_health = perks.max_health;
            return 1;
        }
        default:
            return 0;
    }
}
