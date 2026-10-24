// Client side of the Krunker v7.1.4 map triggers and interfaces (index.js io handlers):
//   "do" Game.destroyObj / "ro" Game.respawnObj / "gte" Game.updateGate / "bnk" Game.updateBank
//     -> NET_PACKET_OBJECT_STATE: the mirrored object state, meshRef.visible, updateShadowMap
//   "chp" checkpointSet -> NET_PACKET_CHECKPOINT: the "Checkpoint!" popup (hud_popups.c)
//   "s" playPlayerSound -> NET_PACKET_SOUND: Game.playerSound -> SOUND.play3D
//   "scr" jumpScare(0) -> NET_PACKET_JUMP_SCARE: #jumpSIMG "/scares/jumpS_2.png" for 3 s + jumpS_1;
//     jumpScare(1): #jumpSVID plays /scares/1.mp4, #jumpSVIDHolder shown for 4 s
//   "am" [text, null] -> NET_PACKET_POPUP: a medal popup without score
//   reconcilePlayer of a doRecon changePosition -> NET_PACKET_POSITION
// WP-C v7 packets (shared.h NET_PACKET_BLOCK_TRIGGERS):
//   "cv" io_customVal -> tmpPlayer.custVals; "gv" io_updateGlobalValue / "gbl" -> Game.globalVals
//   "tgui" io_toggleElement -> the map gui elements' nv (client_triggers_gui_visible)
//   "camb" changeAmbient -> sound_events_change_ambient; "sso" io_stopSound -> SOUND.stop
//   "asnd" / "rsnd" Game.addPlayerSound / removePlayerSound (looping sounds at a player)
//   "cnfm" io_confirmInteraction -> tmpPlayer.pendingTrigger / pendingSpin (ui_interact.c)
//   "chgp" io_changePosition -> Game.players.changePosition(player, ...)
//   the pickup half of "inat" / "pkups" -> game_pickup_set (the interact prompt)
// The offline client's game is the authority itself: it already holds the state, only the
// render side (meshes, shadows, HUD, sounds) follows the events.
#include <map_triggers.h>
#include <world.h> // map-logic WP-B: node moves, check point reset
#include <audio.h>
#include <html_ui.h>
#include <shadow.h>
#include <sound_events.h>
#include <triggers_v7.h>
#include <net_ext.h> // NET_EXT_VALUE_TEXT: a string "cv" / "gv"
#include <ui_logic.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define JUMP_SCARE_IMAGE "scares/jumpS_2.png" // public/scares/2.png of the reference tree
#define JUMP_SCARE_SOUND "scare_1"            // jumpS_1 (public/sound/scare_1.mp3)
#define JUMP_SCARE_TIME 3.0                   // setTimeout(hide, 3000)

static double g_scare_until;
#define JUMP_SCARE_VIDEO "scares/1.mp4"         // jumpScares[1].vid (natively its .knv sidecar, engine/video.h)
#define JUMP_SCARE_VIDEO_TIME 4.0               // setTimeout(hide #jumpSVIDHolder, 4000)
static double g_scare_video_until;

#define CLIENT_PLAYER_SOUND_MAX 32
static struct {
    int uid;
    char name[NET_SOUND_NAME_SIZE];
    int handle;
    int loop;
} g_player_sounds[CLIENT_PLAYER_SOUND_MAX];
static int g_player_sound_count;
// NET_EXT_PLAYER_SOUND_FALLOFF: data.dist / roll of the next PLAYER_SOUND add of `uid` (valid 0 = none)
static struct {
    int valid, uid;
    float distance, rolloff;
} g_sound_falloff;

// The map gui elements' visibility (overlay.customGUIs nv), per element of Map.gui in order.
#define CLIENT_GUI_MAX 256
static struct {
    const Map *map;
    int count;
    int id[CLIENT_GUI_MAX];
    unsigned char type[CLIENT_GUI_MAX];
    unsigned char hidden[CLIENT_GUI_MAX];
} g_gui;

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
        case NET_OBJECT_MOVE_TO_NODE: // "mo": Game.teleportToNode (moves the mesh too)
            if (mirror) world_teleport_object_to_node(&client->game, object, packet->value);
            shadow_load_map(client->game.map);
            return;
        case NET_OBJECT_MOVE_FROM_NODE: // "mf": Game.teleportFromNode
            if (mirror) world_reset_object_position(&client->game, object);
            shadow_load_map(client->game.map);
            return;
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
    if (packet->uid == -2) { // a global trigger sound ("so" name, vol, 1, ..., rate): 2D
        audio_play(name, packet->volume ? packet->volume : 1.0f, 0, packet->rate ? packet->rate : 1.0f);
        return;
    }
    vec3 at = {packet->position[0], packet->position[1], packet->position[2]};
    if (packet->uid >= 0) { // playerSound(sid): only for a known player, at its current position
        const Player *player = game_find_player(&client->game, packet->uid);
        if (!player) return;
        at = player->position;
    }
    audio_play_3d(name, at, packet->volume ? packet->volume : 1.0f, packet->rate, 0.0f, 0.0f); // play3D(a0, x, y, z, a5 || 1, a6)
}

static void jump_scare(const NetJumpScarePacket *packet) {
    if (packet->index != 0 && packet->index != 1) return; // jumpScares[0] the image, [1] the video
    // v10.0.2 window.jumpScare: only with Settings > Game > Customization "Enable Jump Scare" (jumpScare,
    // default on), and at most one every 10 s
    static double last = -1.0e9;
    if (ui_setting_is_false("jumpScare") || glfwGetTime() - last < 10.0) return;
    last = glfwGetTime();
    if (packet->index == 1) {
        // jumpSVID.src = "/scares/1.mp4"; jumpSVID.play(); #jumpSVIDHolder shown for 4 s. play() is the
        // autoplay attribute here: the src change runs the load algorithm, which then plays (natively
        // silent: the engine decodes no audio)
        const HtmlElem video = html_dom_by_id("jumpSVID"), holder = html_dom_by_id("jumpSVIDHolder");
        char url[512];
        html_dom_asset_url(JUMP_SCARE_VIDEO, url, sizeof(url));
        if (video) {
            html_dom_set_attr(video, "autoplay", "");
            html_dom_set_attr(video, "src", url);
        }
        if (holder) html_dom_set_display(holder, "block");
        g_scare_video_until = glfwGetTime() + JUMP_SCARE_VIDEO_TIME;
        return;
    }
    const HtmlElem image = html_dom_by_id("jumpSIMG");
    if (image) {
        html_dom_set_attr(image, "src", JUMP_SCARE_IMAGE);
        html_dom_set_display(image, "block");
    }
    audio_play(JUMP_SCARE_SOUND, 2.0f, 0, 1.0f); // SOUND.play("jumpS_1", 2)
    g_scare_until = glfwGetTime() + JUMP_SCARE_TIME;
}

// changePosition with a direction turns the camera of our own player too (game.deob.js:173813:
// isYou -> controls.object.rotation.y = xDire), so the next input frame carries the new yaw. Ours
// takes the look from me->direction (main.c's resync), which the next "l" overwrites when it
// replays the pending inputs with their old yaw: those take the new yaw as well.
static void turn_pending_inputs(Client *client, const Player *player) {
    if (player != client->me) return;
    for (size_t i = 0; i < client->net_pending_count; i++) client->net_pending_inputs[i].y_dir = player->direction.y;
}

static void change_position(Client *client, const NetPositionPacket *packet) {
    if (client->game.is_local || !client->me) return; // the offline game moved the player itself
    game_change_position(&client->game, client->me, (vec3) {packet->x, packet->y, packet->z}, 0, packet->stop_momentum,
                         packet->has_direction, packet->direction);
    if (packet->has_direction) turn_pending_inputs(client, client->me);
}

// ---------------------------------------------------------------- WP-C v7 packets

// Map.gui elements {id, t, nv, ...} of the current map, read once per map (custom GUI).
static void gui_sync(const Client *client) {
    const Map *map = client->game.ready ? client->game.map : NULL;
    if (g_gui.map == map) return;
    g_gui.map = map;
    g_gui.count = 0;
    const cJSON *list = map && cJSON_IsArray(map->gui) ? map->gui : NULL;
    const cJSON *element;
    cJSON_ArrayForEach(element, list) {
        if (g_gui.count >= CLIENT_GUI_MAX) break;
        const cJSON *id = cJSON_GetObjectItem(element, "id"), *t = cJSON_GetObjectItem(element, "t"), *nv = cJSON_GetObjectItem(element, "nv");
        g_gui.id[g_gui.count] = cJSON_IsNumber(id) ? (int) cJSON_GetNumberValue(id) : 0; // (id || 0)
        g_gui.type[g_gui.count] = (unsigned char) (cJSON_IsNumber(t) ? (int) cJSON_GetNumberValue(t) : 0);
        g_gui.hidden[g_gui.count] = (unsigned char) (cJSON_IsTrue(nv) || (cJSON_IsNumber(nv) && cJSON_GetNumberValue(nv) != 0.0));
        g_gui.count++;
    }
}

int client_triggers_gui_visible(const Client *client, const int index) {
    gui_sync(client);
    return index >= 0 && index < g_gui.count ? !g_gui.hidden[index] : 1;
}

// io_toggleElement(id, status): every element with that id (but type 3): 0 toggle, 1 show, 2 hide
static void gui_toggle(const Client *client, const NetGuiTogglePacket *packet) {
    gui_sync(client);
    for (int i = 0; i < g_gui.count; i++) {
        if (g_gui.id[i] != packet->id || g_gui.type[i] == 3) continue;
        if (packet->status == 0) g_gui.hidden[i] = !g_gui.hidden[i];
        else if (packet->status == 1) g_gui.hidden[i] = 0;
        else if (packet->status == 2) g_gui.hidden[i] = 1;
    }
}

static void player_sound(Client *client, const NetPlayerSoundPacket *packet) {
    char name[NET_SOUND_NAME_SIZE];
    snprintf(name, sizeof(name), "%.*s", (int) sizeof(packet->name) - 1, packet->name);
    if (packet->add) { // addPlayerSound: play3DL / play3D at the player
        const Player *player = game_find_player(&client->game, packet->uid);
        // updatePlayerSounds drops a one-shot once it played (deob-10.0.3.js:220465-220467): the stalker's
        // taunts and a script's one-shots would otherwise fill the list
        for (int i = g_player_sound_count; i-- > 0;) {
            if (g_player_sounds[i].loop || audio_handle_active(g_player_sounds[i].handle)) continue;
            g_player_sounds[i] = g_player_sounds[--g_player_sound_count];
        }
        if (!name[0] || g_player_sound_count >= CLIENT_PLAYER_SOUND_MAX) return;
        const vec3 at = player ? (vec3) {player->position.x, player->position.y + player->height * 0.5f, player->position.z} : (vec3) {0};
        // play3D(val, x, y, z, vol, rate, dist, roll): the falloff the EXT ahead of it carried, else the defaults (0)
        const int falloff = g_sound_falloff.valid && g_sound_falloff.uid == packet->uid;
        g_sound_falloff.valid = 0;
        const int handle = audio_handle_play(name, packet->volume ? packet->volume : 1.0f, packet->loop, packet->rate ? packet->rate : 1.0f, 1, at,
                                             falloff ? g_sound_falloff.distance : 0.0f, falloff ? g_sound_falloff.rolloff : 0.0f);
        g_player_sounds[g_player_sound_count].uid = packet->uid;
        g_player_sounds[g_player_sound_count].loop = packet->loop != 0;
        snprintf(g_player_sounds[g_player_sound_count].name, sizeof(g_player_sounds[0].name), "%s", name);
        g_player_sounds[g_player_sound_count++].handle = handle;
        return;
    }
    g_sound_falloff.valid = 0;
    for (int i = g_player_sound_count; i-- > 0;) { // removePlayerSound(sid, name, force); uid -1 + force: all
        const int match = (packet->uid < 0 || g_player_sounds[i].uid == packet->uid) && (packet->force || !strcmp(g_player_sounds[i].name, name));
        if (!match) continue;
        if (g_player_sounds[i].handle) audio_handle_stop(g_player_sounds[i].handle);
        g_player_sounds[i] = g_player_sounds[--g_player_sound_count];
        if (!packet->force) return;
    }
}

static void change_position_of(Client *client, const NetChangePositionPacket *packet) {
    Player *player = game_find_player(&client->game, packet->uid);
    if (!player || client->game.is_local) return;
    game_change_position(&client->game, player, (vec3) {packet->x, packet->y, packet->z}, 0, packet->stop_momentum, packet->has_direction,
                         packet->direction);
    if (packet->has_direction) turn_pending_inputs(client, player);
    if (player != client->me) {
        player->force_pos = 1; // a remote player snaps instead of interpolating
        player->interp_pos_start = player->interp_pos_end = player->position;
    }
}

#define TRIGGERS_READ(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

// "cv" / "gv" with a string value (NET_EXT_VALUE_TEXT): you.custVals[key] = val / globalVals[key] = val as the
// numbers below (deob-10.0.3.js:258558-258560, :257834-257836), the value kept as the text (triggers.h TriggerValue)
void client_triggers_value_text(Client *client, const void *body, const uint16_t length) {
    NetValueTextPacket packet;
    if (length < sizeof(packet) || client->game.is_local) return;
    memcpy(&packet, body, sizeof(packet));
    packet.key[sizeof(packet.key) - 1] = 0;
    packet.text[sizeof(packet.text) - 1] = 0;
    if (packet.type == NET_PACKET_CUSTOM_VALUE && client->me) trigger_mirror_custom_text(client->me, packet.key, packet.text);
    else if (packet.type == NET_PACKET_GLOBAL_VALUE) trigger_mirror_global_text(&client->game, packet.key, packet.text, packet.flags & NET_VALUE_RESET);
}

int client_triggers_handle_event(Client *client, const int type, const void *payload, const uint16_t length) {
    switch (type) {
        case NET_PACKET_OBJECT_STATE: {
            TRIGGERS_READ(NetObjectStatePacket, packet);
            object_state(client, &packet);
            return 1;
        }
        case NET_PACKET_CHECKPOINT: {
            // io_checkPointSet(reset): v7 servers send NetCheckpointPacket; "chp" true clears the
            // local player's check points (checkPoint, checkPointList, checkPointData)
            NetCheckpointPacket packet = {0};
            if (length >= sizeof(packet)) memcpy(&packet, payload, sizeof(packet));
            if (packet.reset) {
                if (client->me && !client->game.is_local) world_clear_checkpoints(&client->game, client->me);
                return 1;
            }
            hud_popups_checkpoint(); // checkpointSet(): overlay.checkText "Checkpoint!" (showMedals)
            return 1;
        }
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
        case NET_PACKET_CUSTOM_VALUE: {
            TRIGGERS_READ(NetValuePacket, packet);
            packet.key[sizeof(packet.key) - 1] = 0;
            if (client->me && !client->game.is_local) trigger_mirror_custom_value(client->me, packet.key, packet.value);
            return 1;
        }
        case NET_PACKET_GLOBAL_VALUE: {
            TRIGGERS_READ(NetValuePacket, packet);
            packet.key[sizeof(packet.key) - 1] = 0;
            if (!client->game.is_local) trigger_mirror_global_value(&client->game, packet.key, packet.value, packet.flags & NET_VALUE_RESET);
            return 1;
        }
        case NET_PACKET_GUI_TOGGLE: {
            TRIGGERS_READ(NetGuiTogglePacket, packet);
            gui_toggle(client, &packet);
            return 1;
        }
        case NET_PACKET_AMBIENT_CHANGE: {
            TRIGGERS_READ(NetAmbientPacket, packet);
            sound_events_change_ambient(packet.ambient);
            return 1;
        }
        case NET_PACKET_STOP_SOUND: {
            TRIGGERS_READ(NetStopSoundPacket, packet);
            packet.name[sizeof(packet.name) - 1] = 0;
            if (!packet.asset && packet.name[0]) audio_stop(packet.name); // SOUND.stop(name) (no asset sounds natively)
            return 1;
        }
        case NET_PACKET_PLAYER_SOUND: {
            TRIGGERS_READ(NetPlayerSoundPacket, packet);
            player_sound(client, &packet);
            return 1;
        }
        case NET_PACKET_CONFIRM_INTERACT: {
            TRIGGERS_READ(NetConfirmPacket, packet);
            if (!client->me) return 1;
            if (packet.kind == 2) client->me->pending_spin = packet.uid + 1;
            else client->me->pending_trigger = packet.uid + 1; // tmpPlayer.pendingTrigger = uid
            return 1;
        }
        case NET_PACKET_CHANGE_POSITION: {
            TRIGGERS_READ(NetChangePositionPacket, packet);
            change_position_of(client, &packet);
            return 1;
        }
        case NET_PACKET_PICKUP: {
            TRIGGERS_READ(NetPickupPacket, packet);
            const vec3 at = {packet.position[0], packet.position[1], packet.position[2]};
            if (!client->game.is_local) game_pickup_set(&client->game, packet.uid, packet.weapon, packet.active, packet.moved ? &at : NULL, packet.ammo);
            return 1;
        }
        default:
            return 0;
    }
}

void client_triggers_player_sound_falloff(const void *body, const uint16_t length) {
    NetPlayerSoundFalloffPacket packet;
    if (length < sizeof(packet)) return;
    memcpy(&packet, body, sizeof(packet));
    g_sound_falloff.valid = isfinite(packet.distance) && isfinite(packet.rolloff);
    g_sound_falloff.uid = packet.uid;
    g_sound_falloff.distance = packet.distance;
    g_sound_falloff.rolloff = packet.rolloff;
}

// updatePlayerSounds (deob-10.0.3.js:220457-220470): each player sound follows its player, at half its height
static void player_sounds_follow(Client *client) {
    for (int i = 0; i < g_player_sound_count; i++) {
        const Player *player = g_player_sounds[i].handle ? game_find_player(&client->game, g_player_sounds[i].uid) : NULL;
        if (!player) continue;
        audio_handle_position(g_player_sounds[i].handle, (vec3) {player->position.x, player->position.y + player->height * 0.5f, player->position.z});
    }
}

void client_triggers_tick(Client *client) {
    player_sounds_follow(client);
    if (g_scare_video_until && glfwGetTime() >= g_scare_video_until) {
        g_scare_video_until = 0.0;
        const HtmlElem holder = html_dom_by_id("jumpSVIDHolder");
        if (holder) html_dom_set_display(holder, "none");
    }
    if (!g_scare_until || glfwGetTime() < g_scare_until) return;
    g_scare_until = 0.0;
    const HtmlElem image = html_dom_by_id("jumpSIMG");
    if (image) html_dom_set_display(image, "none");
}
