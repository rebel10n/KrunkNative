// Client side of the Krunker v3.1.1 map triggers and interfaces (index.js io handlers):
//   "do" Game.destroyObj / "ro" Game.respawnObj / "gte" Game.updateGate / "bnk" Game.updateBank
//     -> NET_PACKET_OBJECT_STATE: the mirrored object state, meshRef.visible, updateShadowMap
//   "chp" checkpointSet -> NET_PACKET_CHECKPOINT: the "Checkpoint!" popup (hud_popups.c)
//   "s" playPlayerSound -> NET_PACKET_SOUND: Game.playerSound -> SOUND.play3D
//   "scr" jumpScare(0) -> NET_PACKET_JUMP_SCARE: #jumpSIMG "/scares/jumpS_2.png" for 3 s + jumpS_1
//   "am" [text, null] -> NET_PACKET_POPUP: a medal popup without score
//   reconcilePlayer of a doRecon changePosition -> NET_PACKET_POSITION
// The offline client's game is the authority itself: it already holds the state, only the
// render side (meshes, shadows, HUD, sounds) follows the events.
#include <map_triggers.h>
#include <audio.h>
#include <html_ui.h>
#include <shadow.h>
#include <ui_logic.h>
#include <stdio.h>
#include <string.h>

#define JUMP_SCARE_IMAGE "scares/jumpS_2.png" // public/scares/2.png of the reference tree
#define JUMP_SCARE_SOUND "scare_1"            // jumpS_1 (public/sound/scare_1.mp3)
#define JUMP_SCARE_TIME 3.0                   // setTimeout(hide, 3000)

static double g_scare_until;

static Object *trigger_object(Client *client, const int uid) {
    Map *map = client->game.ready ? client->game.map : NULL;
    return map && uid >= 0 && (size_t) uid < map->object_count ? map->objects[uid] : NULL;
}

static void object_state(Client *client, const NetObjectStatePacket *packet) {
    Object *object = trigger_object(client, packet->uid);
    if (!object) return;
    const int mirror = !client->game.is_local;

    switch (packet->kind) {
        case NET_OBJECT_DESTROY:
            if (mirror) {
                object->active = 0;
                object->destroyed = 1;
            }
            break;
        case NET_OBJECT_RESPAWN:
            if (mirror) {
                object->active = 1;
                object->destroyed = 0;
                if (object->start_health) object->health = object->start_health;
            }
            break;
        case NET_OBJECT_GATE:
            if (mirror) object->active = (unsigned char) !!packet->value;
            break;
        case NET_OBJECT_BANK:
            if (mirror) object->deposited = packet->value;
            return; // no mesh change
        default:
            return;
    }

    Mesh *mesh = object->mesh;
    if (mesh && !object->no_vis) mesh->visible = object->active; // meshRef.visible
    shadow_load_map(client->game.map); // RENDER.updateShadowMap()
}

static void play_sound(Client *client, const NetSoundPacket *packet) {
    char name[NET_SOUND_NAME_SIZE];
    snprintf(name, sizeof(name), "%.*s", (int) sizeof(packet->name) - 1, packet->name);
    if (!name[0]) return;
    vec3 at = {packet->position[0], packet->position[1], packet->position[2]};
    if (packet->uid >= 0) { // playerSound(sid): only for a known player, at its current position
        const Player *player = game_find_player(&client->game, packet->uid);
        if (!player) return;
        at = player->position;
    }
    audio_play_3d(name, at, packet->volume ? packet->volume : 1.0f, packet->rate, 0.0f, 0.0f); // play3D(a0, x, y, z, a5 || 1, a6)
}

static void jump_scare(const NetJumpScarePacket *packet) {
    if (packet->index != 0) return; // jumpScares[1] is the jumpS_1.mp4 video: no video playback natively
    const HtmlElem image = html_dom_by_id("jumpSIMG");
    if (image) {
        html_dom_set_attr(image, "src", JUMP_SCARE_IMAGE);
        html_dom_set_display(image, "block");
    }
    audio_play(JUMP_SCARE_SOUND, 2.0f, 0, 1.0f); // SOUND.play("jumpS_1", 2)
    g_scare_until = glfwGetTime() + JUMP_SCARE_TIME;
}

static void change_position(Client *client, const NetPositionPacket *packet) {
    if (client->game.is_local || !client->me) return; // the offline game moved the player itself
    game_change_position(&client->game, client->me, (vec3) {packet->x, packet->y, packet->z}, 0, packet->stop_momentum,
                         packet->has_direction, packet->direction);
}

#define TRIGGERS_READ(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

int client_triggers_handle_event(Client *client, const int type, const void *payload, const uint16_t length) {
    switch (type) {
        case NET_PACKET_OBJECT_STATE: {
            TRIGGERS_READ(NetObjectStatePacket, packet);
            object_state(client, &packet);
            return 1;
        }
        case NET_PACKET_CHECKPOINT:
            hud_popups_checkpoint(); // checkpointSet(): overlay.checkText "Checkpoint!" (showMedals)
            return 1;
        case NET_PACKET_SOUND: {
            TRIGGERS_READ(NetSoundPacket, packet);
            play_sound(client, &packet);
            return 1;
        }
        case NET_PACKET_JUMP_SCARE: {
            TRIGGERS_READ(NetJumpScarePacket, packet);
            jump_scare(&packet);
            return 1;
        }
        case NET_PACKET_POSITION: {
            TRIGGERS_READ(NetPositionPacket, packet);
            change_position(client, &packet);
            return 1;
        }
        case NET_PACKET_POPUP: {
            TRIGGERS_READ(NetPopupPacket, packet);
            packet.text[sizeof(packet.text) - 1] = 0;
            hud_popups_medal_text(packet.text); // addMedal([text, null])
            return 1;
        }
        default:
            return 0;
    }
}

void client_triggers_tick(Client *client) {
    (void) client;
    if (!g_scare_until || glfwGetTime() < g_scare_until) return;
    g_scare_until = 0.0;
    const HtmlElem image = html_dom_by_id("jumpSIMG");
    if (image) html_dom_set_display(image, "none");
}
