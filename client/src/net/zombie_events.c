// map-logic WP-H: the client side of the zombies / airdrop packets (shared/include/zombies.h).
// The state goes into Game.zombies / the local Player through zombies_apply_packet (shared, so
// prediction and the interact prompts read the same data); here are the v7 client's HUD bits:
// io_updateZombieRound ("upz": the "Round N" popup), io_krPackage ("krp": the chat line) and the
// power-up sounds of io_updatePowerUp. The machine / airdrop meshes (client/src/zombies_render.c)
// and the HUD (#powerUpHolder, the finiteAmmo reserve, #roundMessage: client/src/ui/hud_zombies.c)
// read Game.zombies.
#include <client.h>
#include <player_names.h> // Hide player names (player_names.h)
#include <audio.h>
#include <spectate.h>
#include <ui_logic.h>
#include <zombies.h>
#include <stdio.h>
#include <string.h>

#define ZOMBIE_CHAT_YOU "#fff" // COLORS.killFeed.you
#define ZOMBIE_CHAT_ALLY "#9eeb56"
#define ZOMBIE_CHAT_ENEMY "#eb5656"

static const char *player_label(const Client *client, const Player *player, const int own, char *out, const size_t size) {
    if (player == client->me) snprintf(out, size, "%s", own ? "Your" : "You");
    else {
        char pn[PLAYER_NAME_SIZE];
        snprintf(out, size, "%s", player_names_shown(player->uid, 0, player->name, pn, sizeof(pn))); // Hide player names
    }
    return out;
}

// io_krPackage(owner, claimer): "<owner> claimed a package" / "<claimer> stole <owner> package"
static void claim_line(Client *client, const NetAirDropClaimPacket *claim) {
    Player *owner = client_find_player(client, claim->owner_uid);
    if (!owner) return;
    Player *claimer = client_find_player(client, claim->claimer_uid);
    char a[PLAYER_NAME_SIZE + 8], b[PLAYER_NAME_SIZE + 8], line[160];
    const int same = owner == claimer;
    if (same) snprintf(line, sizeof(line), "%s claimed a package", player_label(client, owner, 0, a, sizeof(a)));
    else snprintf(line, sizeof(line), "%s stole %s package", claimer ? player_label(client, claimer, 0, b, sizeof(b)) : "", player_label(client, owner, 1, a, sizeof(a)));
    const int ally = client->me && client->me->team && owner->team == client->me->team;
    hud_ui_add_chat(NULL, line, owner == client->me ? ZOMBIE_CHAT_YOU : ally ? ZOMBIE_CHAT_ALLY : ZOMBIE_CHAT_ENEMY);
}

int zombie_events_handle(Client *client, const int type, const void *payload, const uint16_t length) {
    if (!zombies_apply_packet(&client->game, client->me, type, payload, length)) return 0;
    if (type == NET_PACKET_ZOMBIE_WAVE && length == sizeof(NetZombieWavePacket)) { // io_textPopup(name || "Round", wave)
        NetZombieWavePacket wave;
        memcpy(&wave, payload, sizeof(wave));
        wave.special[sizeof(wave.special) - 1] = 0;
        char text[16];
        snprintf(text, sizeof(text), "%d", (int) wave.wave);
        hud_popups_text_popup(wave.special[0] ? wave.special : "Round", text, 0, 35); // textPopup(special || "Round", wave, null, 35)
        audio_play("mystery_1", 0.2f, 0, 1.0f);
    } else if (type == NET_PACKET_POWERUP && length == sizeof(NetPowerupPacket)) {
        NetPowerupPacket powerup;
        memcpy(&powerup, payload, sizeof(powerup));
        if (powerup.index == ZOMBIES_POWERUP_INSTA_KILL) { // stop + play again (a new pickup restarts it)
            audio_stop("instantkill_0");
            if (powerup.ms > 0) audio_play("instantkill_0", 0.5f, 0, 1.0f);
        }
        if (powerup.index == ZOMBIES_POWERUP_FIRE_SALE) { // the jingle and its 0.1 loop until the sale ends
            audio_stop("firesale_0");
            audio_stop("firesale_1");
            if (powerup.ms > 0) {
                audio_play("firesale_0", 0.5f, 0, 1.0f);
                audio_play("firesale_1", 0.1f, 1, 1.0f);
            }
        }
    } else if (type == NET_PACKET_AIRDROP && length == sizeof(NetAirDropPacket)) {
        NetAirDropPacket drop;
        memcpy(&drop, payload, sizeof(drop));
        if (drop.fall > 0.0f) audio_play("adrop_0", 0.15f, 0, 1.0f);
    } else if (type == NET_PACKET_AIRDROP_CLAIM && length == sizeof(NetAirDropClaimPacket)) {
        NetAirDropClaimPacket claim;
        memcpy(&claim, payload, sizeof(claim));
        claim_line(client, &claim);
    }
    return 1;
}
