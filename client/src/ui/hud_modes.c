// Game mode HUD + client mirror of the mode packets (see client/include/hud_modes.h).
// Krunker v3.1.1 index.js: initGame (#teamScores markup, #livesCount, team names),
// updateTeamScore ("ts"), updateFlagInfo ("ufi"), updateGameMessage ("gmsg"),
// updateTeam ("tm"), updateLives ("lv"), actionFeed ("ac"), lockPlayer ("lock"),
// Game.updateFlags (the CTF flag messages) and the end screen's teamTotal0/1 + endStats.
#include <hud_modes.h>
#include <player_names.h> // Hide player names (player_names.h)
#include <audio.h> // "rds": woosh_0
#include <streamer.h>
#include <mode_world.h>
#include <html_ui.h>
#include <html_dom.h>
#include <ui_logic.h>
#include <stdio.h>
#include <string.h>
#include <spawnables.h>
#include <spectate.h>
#include <cJSON.h>
#include <lobby_ui.h> // the comp menu (comp_ui.c)

Player *client_find_player(Client*, int uid);                      // client/src/main.c
void client_add_player_mesh(Client*, Player*, int render_you);      // client/src/main.c
void scene_remove_player_mesh(Scene*, const PlayerMesh*, size_t);
void player_meshes_fini(Player*);

#define MODES_COLOR_YOU "#fff"       // COLORS.killFeed
#define MODES_COLOR_ALLY "#9eeb56"
#define MODES_COLOR_ENEMY "#eb5656"
#define MODES_TEAM_ENEMY "#eb5656"   // COLORS.teams[1]
#define MODES_HTML_SIZE 4096

// config.js flag messages (Game.updateFlags)
#define MODES_FLAG_MSG "You have the Flag"
#define MODES_FLAG_MSG_E "Enemy has your Flag"
#define MODES_FLAG_MSG_RC "Enemy has your Flag"

typedef struct {
    int sid;
    int team;
    int type;                    // WP-D: medals.js index (spawnables.h SpawnableType)
    int data_c;                  // data.c (KR Tag count)
    vec3 position;
    unsigned char active:1;
} HudModesTag;

typedef struct {
    int mode_id;                     // -1: no round yet
    int team_scores[2];
    unsigned char score_seen[2];     // Game.teamScores has the team (it scored this round): f1996 lists only those
    char flag_info[2][NET_MODE_TEXT_SIZE];
    int lives;                       // -1: the mode has none
    int locked;
    char message[NET_GAME_MESSAGE_SIZE]; // the server's gmsg ("wt" = waiting)
    int flag_count;
    NetModeFlag flags[MODE_MAX_FLAGS];
    HudModesTag tags[MODE_MAX_SPAWNABLES];

    NetModeEndPacket end;
    int end_valid;
    int end_applied;

    // ---- WP-D: v7.1.4 mode state for the HUD (ui-v7 owns the DOM that shows it)
    int team_scores_v7[MODE_TEAMS]; // teams 1..5 (sdm)
    int rounds_left;                // "rds" roundCount, -1 none
    int team_round_wins[MODE_TEAMS];
    int lockout_ms;                 // "lkt"
    int dog_tags;                   // "krt": the local player's KR Tags
    char popup_title[NET_MODE_TEXT_SIZE]; // "tx" (Overtime)
    char popup_text[NET_MODE_TEXT_SIZE];
    int popup_seq;

    // what the DOM shows (rebuilt when these differ)
    char shown_scores[512];
    char shown_message[NET_GAME_MESSAGE_SIZE];
    char shown_lives[256];
    char shown_team[256];
    int shown_blocker;
    int chat_seq;
} HudModes;

static HudModes g_modes = {.mode_id = -1, .lives = -1};
// official "end" rows (NET_EXT_KN_OFFICIAL_END_ROW): the name / values of a row whose player left
static NetOfficialEndRowPacket g_end_rows[NET_MODE_END_MAX];
static int g_end_row_count;

void hud_modes_end_row(const NetOfficialEndRowPacket *row) {
    int i = 0;
    while (i < g_end_row_count && g_end_rows[i].uid != row->uid) i++;
    if (i == g_end_row_count) {
        if (g_end_row_count >= NET_MODE_END_MAX) return;
        g_end_row_count++;
    }
    g_end_rows[i] = *row;
}

static const NetOfficialEndRowPacket *end_row(const int uid) {
    for (int i = 0; i < g_end_row_count; i++)
        if (g_end_rows[i].uid == uid) return &g_end_rows[i];
    return NULL;
}
static char g_scenario_message[NET_GAME_MESSAGE_SIZE]; // --render-scenario modeHud.gameMessage
static int g_scenario_message_set;

static const GameModeConfig *hud_mode_config(const Client *client) {
    return client->game.mode ? &client->game.mode->config : NULL;
}

static int hud_local_team(const Client *client) {
    return client->me ? client->me->team : 0;
}

static void html_escape(const char *in, char *out, const size_t size) {
    size_t len = 0;
    for (const char *c = in ? in : ""; *c && len + 7 < size; c++) {
        const char *rep = *c == '&' ? "&amp;" : *c == '<' ? "&lt;" : *c == '>' ? "&gt;" : *c == '"' ? "&quot;" : *c == '\'' ? "&#39;" : NULL;
        if (rep) {
            const size_t n = strlen(rep);
            memcpy(out + len, rep, n);
            len += n;
        } else {
            out[len++] = *c;
        }
    }
    out[len] = 0;
}

// ------------------------------------------------------------------ packets

static void hud_modes_apply_loadout(Client *client, const NetLoadoutPacket *packet) {
    Player *player = client_find_player(client, packet->uid);
    if (!player) return;

    player->mode.weapon_tier = packet->weapon_tier;
    if (!player->active) return; // the next SPAWN rebuilds the same loadout from the tier

    const int count = packet->count < NET_LOADOUT_MAX ? packet->count : NET_LOADOUT_MAX;
    const int is_you = player == client->me;
    const int had_mesh = player->mesh != NULL;

    // client_drop_player_mesh: the arms are built per loadout slot
    if (had_mesh) {
        scene_remove_player_mesh(player->render_you ? client->fps_scene : client->scene, player->mesh, player->loadout_size);
        player_meshes_fini(player);
    }

    mode_set_player_loadout(player, packet->weapons, count, 0);
    for (int i = 0; i < player->loadout_size && i < count; i++) {
        if (player->ammo && packet->ammo[i] >= 0) player->ammo[i] = (unsigned int) packet->ammo[i]; // -1: the weapon's own (official "inat")
    }

    if (had_mesh) client_add_player_mesh(client, player, is_you);
}

static void hud_modes_chat_line(const char *msg, char *html, size_t size);

static void hud_modes_add_action(Client *client, const NetActionPacket *packet) {
    const Player *actor = client_find_player(client, packet->uid);
    if (!actor || !html_ui_available() || !html_ui_is_ready()) return;

    const Player *other = packet->other_uid >= 0 ? client_find_player(client, packet->other_uid) : NULL;
    const int local_team = hud_local_team(client);
    char name[PLAYER_NAME_SIZE * 6], other_name[PLAYER_NAME_SIZE * 6], text[NET_MODE_TEXT_SIZE * 6];
    char html[1024], left[512], right[512];

    // actionFeed: "You" / the name in the ally or enemy colour, the text, the other player
    if (actor == client->me) {
        snprintf(left, sizeof(left), "<span style='color:%s'>You</span>", MODES_COLOR_YOU);
    } else {
        char shown[PLAYER_NAME_SIZE];
        char pn[PLAYER_NAME_SIZE];
        html_escape(streamer_name(player_names_shown(actor->uid, 0, actor->name, pn, sizeof(pn)), shown, sizeof(shown)), name, sizeof(name)); // fixStr(scrambleS(alias)) in streamer mode
        snprintf(left, sizeof(left), "<span style='color:%s'>%s</span>", local_team && local_team == actor->team ? MODES_COLOR_ALLY : MODES_COLOR_ENEMY, name);
    }

    right[0] = 0;
    if (other == client->me && other) {
        snprintf(right, sizeof(right), "<span style='color:#fff'>You</span>");
    } else if (other) {
        char shown[PLAYER_NAME_SIZE];
        char pn[PLAYER_NAME_SIZE];
        html_escape(streamer_name(player_names_shown(other->uid, 0, other->name, pn, sizeof(pn)), shown, sizeof(shown)), other_name, sizeof(other_name));
        snprintf(right, sizeof(right), "<span style='color:%s'>%s</span>", local_team && local_team == other->team ? MODES_COLOR_ALLY : MODES_COLOR_ENEMY, other_name);
    }

    char raw[NET_MODE_TEXT_SIZE];
    memcpy(raw, packet->text, sizeof(raw));
    raw[sizeof(raw) - 1] = 0;
    html_escape(raw, text, sizeof(text));

    char msg[1024];
    snprintf(msg, sizeof(msg), "%s %s %s", left, text, right);
    hud_modes_chat_line(msg, html, sizeof(html));
}

// io_addChat(-1, null, msg, 1): a plain game line in #chatList (every channel)
static void hud_modes_chat_line(const char *msg, char *html, const size_t size) {
    snprintf(html, size, "<div class='chatItem'><span class='chatMsg'>%s</span></div><br>", msg);
    const HtmlElem list = html_dom_by_id("chatList");
    if (!list) return;
    char id[32];
    snprintf(id, sizeof(id), "chatMsg_m%d", g_modes.chat_seq++);
    const HtmlElem row = html_dom_create("div", id);
    html_dom_set_attr(row, "data-tab", "-1"); // f2018: a game line shows on every chat channel
    html_dom_set_html(row, html);
    html_dom_append(list, row);
    html_dom_scroll_to_bottom(list);
}

// game.updateFlag (the "ufl" packet / the join sync's flg): the client's flag object moves to the
// sent x, y, z and takes the carrier; while carried the packet carries the ping (ufp: img only), so
// the object keeps where it was taken from. The minimap (hud_map.c) draws these objects.
static void hud_modes_mirror_flag(Client *client, const int index) {
    GameMode *mode = client ? client->game.mode : NULL;
    if (!mode || index < 0 || index >= mode->flag_count || index >= MODE_MAX_FLAGS) return;
    const NetModeFlag *in = &g_modes.flags[index];
    ModeFlag *flag = &mode->flags[index];
    flag->carrier_uid = in->carrier_uid;
    flag->uid = in->uid; // the map object id (NET_OBJECT uid), the flag's identity
    flag->active = in->active ? 1 : 0; // an inactive flag is not in play for this mode
    if (in->carrier_uid < 0) flag->position = (vec3) {in->position[0], in->position[1], in->position[2]};
}

static void hud_modes_apply_state(Client *client, const NetModeStatePacket *packet) {
    g_modes.mode_id = packet->mode_id;
    g_modes.team_scores[0] = packet->team_scores[0];
    g_modes.team_scores[1] = packet->team_scores[1];
    // the join sync's getTeamScores lists the teams that scored; KN's state packet carries both
    // scores, so a team with 0 counts as not listed (KN deviation: a team that scored 0 net)
    for (int t = 0; t < 2; t++) g_modes.score_seen[t] = g_modes.score_seen[t] || packet->team_scores[t] != 0;
    for (int t = 0; t < 2; t++) {
        memcpy(g_modes.flag_info[t], packet->flag_info[t], NET_MODE_TEXT_SIZE);
        g_modes.flag_info[t][NET_MODE_TEXT_SIZE - 1] = 0;
    }
    g_modes.lives = packet->lives;
    g_modes.locked = packet->locked;
    memcpy(g_modes.message, packet->message, sizeof(g_modes.message));
    g_modes.message[sizeof(g_modes.message) - 1] = 0;
    g_modes.flag_count = packet->flag_count < MODE_MAX_FLAGS ? packet->flag_count : MODE_MAX_FLAGS;
    memcpy(g_modes.flags, packet->flags, sizeof(g_modes.flags));
    for (int f = 0; f < g_modes.flag_count; f++) hud_modes_mirror_flag(client, f);

    Game *game = &client->game;
    game->move_lock = packet->move_lock;
    if (client->me) {
        client->me->mode.locked = packet->locked;
        if (packet->lives >= 0) client->me->mode.lives = packet->lives;
    }
    if (game->mode) {
        game->mode->team_scores[1] = packet->team_scores[0];
        game->mode->team_scores[2] = packet->team_scores[1];
        if (game->mode->config.objective) game->mode->active_objective = packet->active_objective;
        if (game->mode->config.obj_weapon) game->mode->active_objective = packet->objective_weapon;
    }
}

// ---- WP-D: the v7.1.4 mode packets (block 140-149) into the client's GameMode mirror
static int hud_modes_spawnable_slot(const int sid) {
    for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) {
        if (g_modes.tags[i].active && g_modes.tags[i].sid == sid) return i;
    }
    return -1;
}

static int hud_modes_handle_v7(Client *client, const int packet_type, const void *payload, const uint16_t length) {
#define V7_READ(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

    Game *game = &client->game;
    GameMode *mode = game->mode;

    switch (packet_type) {
        case NET_PACKET_OBJECTIVE_V7: { // setObjective(uid, time)
            V7_READ(NetObjectiveV7Packet, obj);
            if (mode) {
                mode->active_objective = obj.uid;
                mode->objective_time = (float) obj.time_ms / 1000.0f;
                for (int i = 0; i < mode->objective_count; i++) mode->objectives[i].on_zone = 0;
            }
            return 1;
        }
        case NET_PACKET_OBJECTIVE_OCCUPANCY: { // updateObjective(uid, onZone)
            V7_READ(NetObjectiveOccupancyPacket, occupancy);
            for (int i = 0; mode && i < mode->objective_count; i++) {
                if (mode->objectives[i].uid == occupancy.uid) mode->objectives[i].on_zone = occupancy.on_zone;
            }
            return 1;
        }
        case NET_PACKET_DOM_UPDATE: { // updateDomZone(...) per zone
            V7_READ(NetDomUpdatePacket, update);
            for (int i = 0; mode && i < update.count && i < MODE_MAX_DOM_ZONES; i++) {
                const NetDomZoneState *in = &update.zones[i];
                if (in->uid < 0 || in->uid >= mode->dom_zone_count) continue;
                ModeDomZone *zone = &mode->dom_zones[in->uid];
                zone->captured_by = in->captured_by;
                zone->capture_t = in->capture_t / 1000.0f;
                zone->last_team_on = in->last_team_on;
                zone->object_id = in->object_id; // the map object id, kept with the zone
                zone->teams_on_count = in->teams_on_count < MODE_TEAMS ? in->teams_on_count : MODE_TEAMS;
                for (int t = 0; t < zone->teams_on_count; t++) zone->teams_on[t] = in->teams_on[t];
                zone->mlt = in->mlt > 0.0f ? in->mlt : 1.0f;
            }
            return 1;
        }
        case NET_PACKET_DOM_CAPTURE: {
            V7_READ(NetDomCapturePacket, capture);
            if (mode && capture.uid >= 0 && capture.uid < mode->dom_zone_count) {
                // io_captureControl: updateDomZone(uid, team, 0, team, [team], 1), then the chat line
                // "Zone X Captured" in killFeed.ally / enemy (the viewer's team: 1 while spectating)
                ModeDomZone *zone = &mode->dom_zones[capture.uid];
                zone->captured_by = capture.team;
                zone->capture_t = 0.0f;
                zone->last_team_on = capture.team;
                zone->mlt = 1.0f;
                const int viewer = client->me && client->me->team ? client->me->team : spectate_active() ? 1 : 0;
                if (html_ui_available() && html_ui_is_ready()) {
                    char msg[160], html[512];
                    snprintf(msg, sizeof(msg), "<span style='color:%s'>Zone %c Captured</span>",
                             capture.team == viewer ? MODES_COLOR_ALLY : MODES_COLOR_ENEMY, 'A' + capture.uid % 26);
                    hud_modes_chat_line(msg, html, sizeof(html));
                }
            }
            return 1;
        }
        case NET_PACKET_ROUNDS: { // io_updateRoundsDis: "Switching Sides" + the lead
            V7_READ(NetRoundsPacket, rounds);
            g_modes.rounds_left = rounds.rounds_left;
            for (int t = 0; t < MODE_TEAMS; t++) g_modes.team_round_wins[t] = rounds.team_round_wins[t];
            if (mode) {
                mode->round_count = rounds.rounds_left;
                mode->has_round_wins = rounds.has_round_wins;
                for (int t = 0; t < MODE_TEAMS; t++) mode->team_round_wins[t] = rounds.team_round_wins[t];
                mode->objective_time = 0.0f;
                mode->flipped_spawns = !mode->flipped_spawns;
            }
            char lead[64];
            if (rounds.win_team > 0) snprintf(lead, sizeof(lead), "%s is in the Lead!", mode_team_name(game, rounds.win_team));
            else snprintf(lead, sizeof(lead), "Both Teams are Equal!");
            snprintf(g_modes.popup_title, sizeof(g_modes.popup_title), "Switching Sides");
            audio_play("woosh_0", 0.5f, 0, 1.0f); // SOUND.play("woosh_0", 0.5) (:219260)
            snprintf(g_modes.popup_text, sizeof(g_modes.popup_text), "%s", lead);
            g_modes.popup_seq++;
            hud_popups_text_popup(g_modes.popup_title, g_modes.popup_text, 1, 0); // textPopup("Switching Sides", lead, true) (:219261)
            return 1;
        }
        case NET_PACKET_LOCKOUT: {
            V7_READ(NetLockoutPacket, lockout);
            g_modes.lockout_ms = lockout.ms;
            if (mode) mode->lockout_timer = (float) lockout.ms / 1000.0f;
            return 1;
        }
        case NET_PACKET_TEXT_POPUP: { // io_textPopup(title, text): infoText / infoSub
            V7_READ(NetTextPopupPacket, popup);
            memcpy(g_modes.popup_title, popup.title, sizeof(g_modes.popup_title));
            memcpy(g_modes.popup_text, popup.text, sizeof(g_modes.popup_text));
            g_modes.popup_title[sizeof(g_modes.popup_title) - 1] = 0;
            g_modes.popup_text[sizeof(g_modes.popup_text) - 1] = 0;
            g_modes.popup_seq++;
            hud_popups_text_popup(g_modes.popup_title, g_modes.popup_text, 0, 0);
            return 1;
        }
        case NET_PACKET_MODE_COUNTER: {
            V7_READ(NetModeCounterPacket, counter);
            Player *player = client_find_player(client, counter.uid);
            if (counter.kind == NET_MODE_COUNTER_KR_TAGS) {
                g_modes.dog_tags = (int) counter.value;
                if (player) player->mode.dog_tags = (int) counter.value;
            } else if (counter.kind == NET_MODE_COUNTER_CHARGE && player) { // "chrg": the kranked timer (game-overlay)
                player->mode.is_kranked = counter.value > 0.0f;
                player->mode.kranked_timer = counter.value / 1000.0f;
            } else if (counter.kind == NET_MODE_COUNTER_HEAD && player) { // "bh": updateHeadSize
                player->mode.head_mlt = counter.value;
            }
            return 1;
        }
        case NET_PACKET_SPAWNABLE_V7: { // createSpawnable / disposeSpawnable
            V7_READ(NetSpawnableV7Packet, spawnable);
            int slot = hud_modes_spawnable_slot(spawnable.sid);
            if (!spawnable.active) {
                if (slot >= 0) g_modes.tags[slot].active = 0;
                return 1;
            }
            for (int i = 0; slot < 0 && i < MODE_MAX_SPAWNABLES; i++) {
                if (!g_modes.tags[i].active) slot = i;
            }
            if (slot >= 0) {
                g_modes.tags[slot] = (HudModesTag) {
                    .sid = spawnable.sid, .team = spawnable.team, .type = spawnable.type, .data_c = spawnable.data_c, .active = 1,
                    .position = {spawnable.position[0], spawnable.position[1], spawnable.position[2]},
                };
            }
            return 1;
        }
        case NET_PACKET_MODE_SYNC_V7: { // init teamScores 1..5, roundC, lockT, obj
            V7_READ(NetModeSyncV7Packet, sync);
            for (int t = 1; t < MODE_TEAMS; t++) {
                g_modes.team_scores_v7[t] = sync.team_scores[t];
                g_modes.team_round_wins[t] = sync.team_round_wins[t];
                if (t <= 2) g_modes.team_scores[t - 1] = sync.team_scores[t];
            }
            g_modes.rounds_left = sync.rounds_left;
            g_modes.lockout_ms = sync.lockout_ms;
            if (mode) {
                for (int t = 1; t < MODE_TEAMS; t++) {
                    mode->team_scores[t] = sync.team_scores[t];
                    mode->team_round_wins[t] = sync.team_round_wins[t];
                }
                mode->round_count = sync.rounds_left;
                mode->has_round_wins = sync.has_round_wins;
                mode->lockout_timer = (float) sync.lockout_ms / 1000.0f;
                if (mode->config.objective) {
                    mode->active_objective = sync.objective_uid;
                    mode->objective_time = (float) sync.objective_ms / 1000.0f;
                }
            }
            return 1;
        }
        default:
            return 0;
    }
#undef V7_READ
}

int hud_modes_handle_packet(Client *client, const NetPacket *packet) {
    return hud_modes_handle_event(client, packet->type, packet->payload, packet->length);
}

// The typed form: the offline client's game hands its events over as the structs themselves.
int hud_modes_handle_event(Client *client, const int packet_type, const void *payload, const uint16_t length) {
#define MODES_READ(type, var) \
    type var; \
    if (length != sizeof(type)) return 1; \
    memcpy(&var, payload, sizeof(var))

    Game *game = &client->game;

    switch (packet_type) {
        case NET_PACKET_MODE_STATE: {
            MODES_READ(NetModeStatePacket, state);
            hud_modes_apply_state(client, &state);
            return 1;
        }
        case NET_PACKET_GAME_MESSAGE: {
            MODES_READ(NetGameMessagePacket, message);
            memcpy(g_modes.message, message.text, sizeof(g_modes.message));
            g_modes.message[sizeof(g_modes.message) - 1] = 0;
            return 1;
        }
        case NET_PACKET_TEAM: {
            MODES_READ(NetTeamPacket, team);
            Player *player = client_find_player(client, team.uid);
            if (player) {
                player->team = team.team;
                player->max_health = team.max_health > 0 ? team.max_health : player->max_health;
                player->health = team.health;
                game->leaders_dirty = 1;
                // a new role's outfit (Infected / Stalker): rebuild the model
                if (mode_apply_outfit(game, player) && player->mesh && player->active) {
                    const int is_you = player == client->me;
                    scene_remove_player_mesh(player->render_you ? client->fps_scene : client->scene, player->mesh, player->loadout_size);
                    player_meshes_fini(player);
                    client_add_player_mesh(client, player, is_you);
                }
            }
            return 1;
        }
        case NET_PACKET_TEAM_SCORE: {
            MODES_READ(NetTeamScorePacket, score);
            if (score.team == 0) { // an official server's "ts" without a team (krunker_link.c): teamScores = {}
                memset(g_modes.team_scores, 0, sizeof(g_modes.team_scores));
                memset(g_modes.team_scores_v7, 0, sizeof(g_modes.team_scores_v7));
                memset(g_modes.score_seen, 0, sizeof(g_modes.score_seen));
                if (game->mode) memset(game->mode->team_scores, 0, sizeof(game->mode->team_scores));
                return 1;
            }
            if (score.team >= 1 && score.team <= 2) {
                g_modes.team_scores[score.team - 1] = score.score;
                g_modes.score_seen[score.team - 1] = 1; // io_teamScore: Game.teamScores[team] = score
            }
            if (score.team >= 1 && score.team < MODE_TEAMS) { // v7: sdm teams 3..5 too
                g_modes.team_scores_v7[score.team] = score.score;
                if (game->mode) game->mode->team_scores[score.team] = score.score;
            }
            return 1;
        }
        case NET_PACKET_FLAG: {
            MODES_READ(NetFlagPacket, flag);
            // official servers (krunker_link.c) send the flag line ("ufi") and the flag ("ufl" / "ufp")
            // apart: index -1 is the team's line only; team 0 keeps the flag's team (else the map's), an
            // empty info keeps the line
            if (flag.index == -1 && flag.flag.team >= 1 && flag.flag.team <= 2) {
                memcpy(g_modes.flag_info[flag.flag.team - 1], flag.info, NET_MODE_TEXT_SIZE);
                g_modes.flag_info[flag.flag.team - 1][NET_MODE_TEXT_SIZE - 1] = 0;
                return 1;
            }
            if (flag.index >= 0 && flag.index < MODE_MAX_FLAGS) {
                if (!flag.flag.team) {
                    const GameMode *mode = game->mode;
                    flag.flag.team = g_modes.flags[flag.index].team ? g_modes.flags[flag.index].team
                                     : mode && flag.index < mode->flag_count ? mode->flags[flag.index].team : 0;
                }
                g_modes.flags[flag.index] = flag.flag;
                if (flag.index >= g_modes.flag_count) g_modes.flag_count = flag.index + 1;
                hud_modes_mirror_flag(client, flag.index);
                if (flag.flag.team >= 1 && flag.flag.team <= 2 && flag.info[0]) {
                    memcpy(g_modes.flag_info[flag.flag.team - 1], flag.info, NET_MODE_TEXT_SIZE);
                    g_modes.flag_info[flag.flag.team - 1][NET_MODE_TEXT_SIZE - 1] = 0;
                }
            }
            return 1;
        }
        case NET_PACKET_OBJECTIVE: {
            MODES_READ(NetObjectivePacket, objective);
            if (game->mode) game->mode->active_objective = objective.value;
            return 1;
        }
        case NET_PACKET_LIVES: {
            MODES_READ(NetLivesPacket, lives);
            g_modes.lives = lives.lives;
            if (client->me) client->me->mode.lives = lives.lives;
            return 1;
        }
        case NET_PACKET_ACTION: {
            MODES_READ(NetActionPacket, action);
            hud_modes_add_action(client, &action);
            return 1;
        }
        case NET_PACKET_LOADOUT: {
            MODES_READ(NetLoadoutPacket, loadout);
            hud_modes_apply_loadout(client, &loadout);
            return 1;
        }
        case NET_PACKET_LOCK: {
            MODES_READ(NetLockPacket, lock);
            if (lock.kind == NET_LOCK_MOVE) {
                game->move_lock = lock.locked;
            } else {
                g_modes.locked = lock.locked;
                if (client->me) client->me->mode.locked = lock.locked;
            }
            return 1;
        }
        case NET_PACKET_SPAWNABLE: {
            MODES_READ(NetSpawnablePacket, tag);
            int slot = -1;
            for (int i = 0; i < MODE_MAX_SPAWNABLES; i++) {
                if (g_modes.tags[i].active && g_modes.tags[i].sid == tag.sid) slot = i;
            }
            if (!tag.active) {
                if (slot >= 0) g_modes.tags[slot].active = 0;
                return 1;
            }
            for (int i = 0; slot < 0 && i < MODE_MAX_SPAWNABLES; i++) {
                if (!g_modes.tags[i].active) slot = i;
            }
            if (slot >= 0) {
                g_modes.tags[slot] = (HudModesTag) {
                    .sid = tag.sid, .team = tag.team, .active = 1,
                    .position = {tag.position[0], tag.position[1], tag.position[2]},
                };
            }
            return 1;
        }
        case NET_PACKET_MODE_END: {
            MODES_READ(NetModeEndPacket, end);
            g_modes.end = end;
            g_modes.end_valid = 1;
            g_modes.end_applied = 0;
            g_modes.team_scores[0] = end.team_scores[0];
            g_modes.team_scores[1] = end.team_scores[1];
            return 1;
        }
        case NET_PACKET_MEDAL: {
            MODES_READ(NetMedalPacket, medal);
            hud_popups_medals(&medal, 1); // "am" -> gm([text, score])
            return 1;
        }
        default:
            return hud_modes_handle_v7(client, packet_type, payload, length);
    }
#undef MODES_READ
}

void hud_modes_reset(Client *client) {
    mode_world_unload(client);
    g_end_row_count = 0;
    const int chat_seq = g_modes.chat_seq;
    memset(&g_modes, 0, sizeof(g_modes));
    g_modes.mode_id = -1;
    g_modes.lives = -1;
    g_modes.rounds_left = -1;
    g_modes.shown_blocker = -1;
    g_modes.chat_seq = chat_seq;
    snprintf(g_modes.flag_info[0], NET_MODE_TEXT_SIZE, "AT BASE");
    snprintf(g_modes.flag_info[1], NET_MODE_TEXT_SIZE, "AT BASE");
    // force a DOM rewrite
    snprintf(g_modes.shown_scores, sizeof(g_modes.shown_scores), "?");
    snprintf(g_modes.shown_message, sizeof(g_modes.shown_message), "?");
    snprintf(g_modes.shown_lives, sizeof(g_modes.shown_lives), "?");
    snprintf(g_modes.shown_team, sizeof(g_modes.shown_team), "?");
}

// ------------------------------------------------------------------ DOM

static void dom_set_display(const char *id, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return;
    char current[32];
    if (html_dom_get_style(el, "display", current, sizeof(current)) < 0 || strcmp(current, value)) html_dom_set_display(el, value);
}

// f1996's #teamScores rows: CTF shows the score, the flag icon and the flag status; the other
// team modes a team colour square and the score. The local team's icon gets "you" (blue flag /
// white square). Only the teams in Game.teamScores (those that scored this round) get a row, and
// #teamScores stays hidden until one has (v7 starts every round with teamScores = {}).
static void hud_modes_scores(const Client *client, const GameModeConfig *config) {
    if (!config || !config->teams || !(g_modes.score_seen[0] || g_modes.score_seen[1])) {
        dom_set_display("teamScores", "none");
        g_modes.shown_scores[0] = 0;
        return;
    }

    const int team = hud_local_team(client);
    char html[MODES_HTML_SIZE];
    size_t len = 0;
    html[0] = 0;

    for (int t = 1; t <= 2; t++) {
        if (!g_modes.score_seen[t - 1]) continue;
        const int score = g_modes.team_scores[t - 1];
        if (config->flags) {
            char info[NET_MODE_TEXT_SIZE * 6];
            html_escape(g_modes.flag_info[t - 1][0] ? g_modes.flag_info[t - 1] : "AT BASE", info, sizeof(info));
            len += (size_t) snprintf(html + len, sizeof(html) - len,
                "<div class='tScore'><span class='tScoreT' id='tScoreV%d'>%d</span><i class='material-icons tScoreF%s' id='tScoreF%d'>flag</i><span class='tScoreM' id='tScoreM%d'>%s</span></div>",
                t, score, team == t ? " you" : "", t, t, info);
        } else {
            len += (size_t) snprintf(html + len, sizeof(html) - len,
                "<div class='tScore'><div id='tScoreC%d' class='tScoreC%s'></div><span class='tScoreT' id='tScoreV%d'>%d</span></div>",
                t, team == t ? " you" : "", t, score);
        }
        if (len >= sizeof(html)) break;
    }

    const HtmlElem el = html_dom_by_id("teamScores");
    if (!el) return;

    char key[sizeof(g_modes.shown_scores)];
    snprintf(key, sizeof(key), "%.*s", (int) sizeof(key) - 1, html);
    if (strcmp(key, g_modes.shown_scores) || !html_dom_by_id("tScoreV1")) {
        html_dom_set_html(el, html);
        snprintf(g_modes.shown_scores, sizeof(g_modes.shown_scores), "%s", key);
    }
    dom_set_display("teamScores", "inline-block");
}

// Game.updateFlags' #gameMessage for the local player (CTF), else the server's gmsg.
const char *hud_modes_game_message(void) {
    return g_modes.message;
}

static void hud_modes_message(const Client *client, const GameModeConfig *config) {
    const char *text = g_scenario_message_set ? g_scenario_message : g_modes.message;
    char waiting[NET_GAME_MESSAGE_SIZE * 2];
    if (!strcmp(text, NET_GAME_MESSAGE_WAITING)) {
        text = I18N("player.waiting", "waiting for players");
    } else if (!strncmp(text, NET_GAME_MESSAGE_WAITING " ", 3)) { // an official "gmsg" ["wt", suffix]: t(player.waiting) + " " + suffix
        snprintf(waiting, sizeof(waiting), "%s %s", I18N("player.waiting", "waiting for players"), text + 3);
        text = waiting;
    }

    if (config && config->flags && client->me) {
        int carrying = 0, enemy_has_ours = 0;
        for (int f = 0; f < g_modes.flag_count; f++) {
            const NetModeFlag *flag = &g_modes.flags[f];
            if (!flag->active || flag->carrier_uid < 0) continue;
            if (flag->team == client->me->team) enemy_has_ours = 1;
            if (flag->carrier_uid == client->me->uid) carrying = 1;
        }
        text = enemy_has_ours && carrying ? MODES_FLAG_MSG_RC : enemy_has_ours ? MODES_FLAG_MSG_E : carrying ? MODES_FLAG_MSG : text;
    }

    comp_ui_message(text); // io_updateGameMessage: compTimBox.textContent
    const HtmlElem el = html_dom_by_id("gameMessage");
    if (!el) return;
    char current[NET_GAME_MESSAGE_SIZE];
    html_dom_get_text(el, current, sizeof(current));
    if (strcmp(current, text)) html_dom_set_text(el, text);
}

// initGame's #livesCount (config.lives || mode.lives) and updateLives' #livesDisp.
static void hud_modes_lives(const GameModeConfig *config) {
    if (!config || g_modes.lives < 0) {
        dom_set_display("livesCount", "none");
        return;
    }

    char html[256];
    snprintf(html, sizeof(html), "<i class='material-icons' style='color:#fff;font-size:35px'>favorite</i> <span id='livesDisp' style='color:rgba(255,255,255,0.6)'>%d</span>", g_modes.lives);
    const HtmlElem el = html_dom_by_id("livesCount");
    if (!el) return;
    if (strcmp(html, g_modes.shown_lives) || !html_dom_by_id("livesDisp")) {
        html_dom_set_html(el, html);
        snprintf(g_modes.shown_lives, sizeof(g_modes.shown_lives), "%s", html);
    }
    dom_set_display("livesCount", "inline-block");
}

// mode.showTeam: the #teamName pill with teamOptions[team - 1] (teamColors or 60% black).
static void hud_modes_team_name(const Client *client, const GameModeConfig *config) {
    const HtmlElem el = html_dom_by_id("teamName");
    if (!el) return;

    const int team = hud_local_team(client);
    char key[256];
    if (config && config->show_team && config->team_options[0] && team >= 1 && team <= 2) {
        snprintf(key, sizeof(key), "%s|%s", config->team_options[team - 1], config->team_colors[0] ? config->team_colors[team - 1] : "rgba(0,0,0,0.6)");
    } else {
        key[0] = 0;
    }

    char opacity[16];
    const int opacity_set = html_dom_get_style(el, "opacity", opacity, sizeof(opacity)) >= 0;
    const char *want_opacity = key[0] ? "1" : "0";
    if (!strcmp(key, g_modes.shown_team) && opacity_set && !strcmp(opacity, want_opacity)) return;

    snprintf(g_modes.shown_team, sizeof(g_modes.shown_team), "%s", key);
    html_dom_set_style(el, "opacity", want_opacity);
    if (!key[0]) return;

    html_dom_set_style(el, "background-color", config->team_colors[0] ? config->team_colors[team - 1] : "rgba(0,0,0,0.6)");
    html_dom_set_html(el, config->team_options[team - 1]);
}

// lockPlayer: #blockerDiv covers the screen of a locked seeker.
static void hud_modes_blocker(const Client *client) {
    const int locked = client->me && client->me->active && g_modes.locked;
    dom_set_display("blockerDiv", locked ? "block" : "none");
}

// ------------------------------------------------------------------ end screen

static const char *hud_modes_end_header(const char *stat) {
    if (!strcmp(stat, "objective")) return "Objective";
    if (!strcmp(stat, "MMR")) return "MMR";
    static char header[32];
    snprintf(header, sizeof(header), "%s", stat);
    if (header[0] >= 'a' && header[0] <= 'z') header[0] = (char) (header[0] - 'a' + 'A'); // UTILS.capFirst
    return header;
}

static const NetModeEndEntry *hud_modes_end_entry(const int uid) {
    for (int i = 0; i < g_modes.end.count && i < NET_MODE_END_MAX; i++) {
        if (g_modes.end.entries[i].uid == uid) return &g_modes.end.entries[i];
    }
    return NULL;
}

// globalConfig.endForm and the plain Player fields of the endStats keys.
static void hud_modes_end_value(const Player *player, const NetModeEndEntry *entry, const char *stat, char *out, const size_t size) {
    const int16_t *s = entry ? entry->stats : NULL;
    if (!strcmp(stat, "score")) snprintf(out, size, "%d", player->score);
    else if (!strcmp(stat, "kills")) snprintf(out, size, "%u", player->kills);
    else if (!strcmp(stat, "deaths")) snprintf(out, size, "%u", player->deaths);
    else if (!strcmp(stat, "assists")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_ASSISTS] : (int) player->assists);
    else if (!strcmp(stat, "reward")) {                                   // endForm.reward: player.reward (END rewards)
        if (player->reward) snprintf(out, size, "<span style='color:#F8C55C'>+%d</span> KR", player->reward);
        else snprintf(out, size, "NONE");
    }
    else if (!strcmp(stat, "objective")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_OBJECTIVE] : 0);
    else if (!strcmp(stat, "caps")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_CAPS] : 0);
    else if (!strcmp(stat, "confirms")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_CONFIRMS] : 0);
    else if (!strcmp(stat, "denies")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_DENIES] : 0);
    else if (!strcmp(stat, "weapon")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_WEAPON_TIER] : 0); // endForm.weapon: weaponTier
    else if (!strcmp(stat, "melees")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_MELEES] : 0);
    else if (!strcmp(stat, "setbacks")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_SETBACKS] : 0);
    else if (!strcmp(stat, "infected") || !strcmp(stat, "found")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_CONVS] : 0);
    else if (!strcmp(stat, "obj")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_OBJECTIVE] : 0);   // v7 endForm.obj: objectiveScore
    else if (!strcmp(stat, "deposits")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_DEPOSITS] : 0); // v7 Deposit (WP-D)
    else if (!strcmp(stat, "pickups")) snprintf(out, size, "%d", s ? s[NET_MODE_STAT_PICKUPS] : 0);   // v7 Mag Dump (WP-D)
    else if (!strcmp(stat, "zombies")) snprintf(out, size, "%d", player->mode.ai_kills); // endForm.zombies: aiKills (NET_PACKET_ZOMBIE_STATS)
    else if (!strcmp(stat, "downs")) snprintf(out, size, "%u", player->deaths);         // endForm.downs: deaths
    else if (!strcmp(stat, "rounds")) snprintf(out, size, "%d", player->mode.rounds);   // player.rounds (NET_PACKET_ZOMBIE_STATS)
    else if (!strcmp(stat, "time")) {                                     // endForm.time: timer || "DNF"
        const int tenths = s ? s[NET_MODE_STAT_TIME] - 1 : -1;            // UTILS.getTime(lastTimer, showMS): Race / Raid
        if (tenths < 0) snprintf(out, size, "DNF");
        else snprintf(out, size, "%02d:%02d.%d", tenths / 600 % 60, tenths / 10 % 60, tenths % 10);
    }
    else snprintf(out, size, "0");
}

static const char *const k_default_end_stats[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL};

const char *const *hud_modes_end_stats(const Client *client) {
    const GameModeConfig *config = client ? hud_mode_config(client) : NULL;
    return config && config->end_stats ? config->end_stats : k_default_end_stats;
}

int hud_modes_end_rows(int *uids, const int max) {
    if (!g_modes.end_valid) return 0;
    int n = 0;
    for (int i = 0; i < g_modes.end.count && i < NET_MODE_END_MAX && n < max; i++) uids[n++] = g_modes.end.entries[i].uid;
    return n;
}

void hud_modes_end_cell(const Player *player, const char *stat, char *out, const size_t size) {
    if (!player || !stat || !out || !size) return;
    hud_modes_end_value(player, hud_modes_end_entry(player->uid), stat, out, size);
}

int hud_modes_end_team_score(const int team) {
    return g_modes.end_valid && (team == 1 || team == 2) ? g_modes.end.team_scores[team - 1] : 0;
}

int hud_modes_end_winner_team(void) {
    if (!g_modes.end_valid) return 0;
    return g_modes.end.winner_team > 0 ? g_modes.end.winner_team : -1;
}

// The live #endTable's markup (f1917 / v10 showTable with the mode's endStats): the header row and the
// rows of g_modes.end in the server's endSort order, without the <table> tag (html: size bytes). 0 when
// there is no MODE_END packet. The end screen writes it into #endTable (hud_modes_end_screen) and the
// Leaderboard tab (hud_ui.c hud_end_leaderboard_html) wraps the same markup, so both show one table.
int hud_modes_end_table_html(const Client *client, char *html, const size_t size) {
    if (!g_modes.end_valid || !client || !html || size < 2048) return 0;
    html[0] = 0;
    const GameModeConfig *config = hud_mode_config(client);
    const char *const *stats = config && config->end_stats ? config->end_stats : k_default_end_stats;
    int stat_count = 0;
    while (stats[stat_count]) stat_count++;

    size_t len = 0;
    // v7 f1917: <th id='endlb_<stat>'> headers and a 20px spacer row
    len += (size_t) snprintf(html + len, size - len, "<tr><th id='endlb_rank'>#</th>"); // v10:219618 endlb_rank "#"
    for (int i = 1; i < stat_count; i++) {
        char id[32];
        size_t k = 0;
        for (const char *c = stats[i]; *c && k + 1 < sizeof(id); c++) id[k++] = (char) (*c >= 'A' && *c <= 'Z' ? *c + 32 : *c);
        id[k] = 0;
        len += (size_t) snprintf(html + len, size - len, "<th id='endlb_%s'>%s</th>", id, hud_modes_end_header(stats[i]));
    }
    // the spacer row draws no rule: v10's `th, tr { border-bottom }` would give it one in any engine that paints row borders
    len += (size_t) snprintf(html + len, size - len, "<th></th></tr><tr height='20px;' style='border-bottom:0'></tr>");

    // v10:219638: localTeam = me.team || (spectating ? 1 : 0). A teamless (FFA) player's team is null
    // there, so `localTeam == team` is false and every other FFA row takes the enemy colour.
    const int local_team = client->me && client->me->team ? client->me->team : spectate_active() ? 1 : 0;
    for (int r = 0; r < g_modes.end.count && r < NET_MODE_END_MAX && len + 1400 < size; r++) {
        const NetModeEndEntry *entry = &g_modes.end.entries[r];
        const Player *player = client_find_player((Client *) client, entry->uid);
        // an official row prints its player even after it left (v10 :219871-219890: the row's own
        // [name, alias] cell and values); our own server's rows are all of present players
        const NetOfficialEndRowPacket *row = end_row(entry->uid);
        static Player ghost;
        if (!player && row) {
            memset(&ghost, 0, sizeof(ghost));
            ghost.uid = row->uid;
            snprintf(ghost.name, sizeof(ghost.name), "%s", row->name);
            ghost.score = row->score;
            ghost.kills = (unsigned int) (row->kills > 0 ? row->kills : 0);
            ghost.deaths = (unsigned int) (row->deaths > 0 ? row->deaths : 0);
            ghost.reward = row->reward;
        }
        const int left = !player;
        if (left && !row) continue;
        if (left) player = &ghost;

        // showEndTable: the link keeps the real name, the text is scrambleS(alias) in streamer mode
        char name[PLAYER_NAME_SIZE * 6], shown_name[PLAYER_NAME_SIZE * 6], shown[PLAYER_NAME_SIZE];
        char pn[PLAYER_NAME_SIZE];
        const char *raw_name = player_names_shown(player->uid, player == client->me, player->name, pn, sizeof(pn)); // Hide player names
        html_escape(raw_name, name, sizeof(name));
        html_escape(streamer_name(raw_name, shown, sizeof(shown)), shown_name, sizeof(shown_name));
        const char *color = player == client->me ? "#fff" : !left && player->team && local_team == player->team ? "rgba(255,255,255,0.6)" : MODES_TEAM_ENEMY;

        len += (size_t) snprintf(html + len, size - len, "<tr style='%s'><td style='color:#fff'>%d.</td>",
                                 r + 1 >= g_modes.end.count ? "border-bottom:none" : "", r + 1);
        for (int i = 1; i < stat_count; i++) {
            if (!strcmp(stats[i], "name") && player_names_hidden()) { // no profile link while names are hidden (it is the real account's)
                len += (size_t) snprintf(html + len, size - len, "<td style='position: relative;'><img class='endTablePfp' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='./img/player_pfp.png'>"
                                         "<span class='endTableN' style='color:%s'>%s</span></td>", color, shown_name);
            } else if (!strcmp(stats[i], "name")) {
                len += (size_t) snprintf(html + len, size - len, "<td style='position: relative;'><img class='endTablePfp' onerror='this.onerror=null;this.src=\"./img/player_pfp.png\";'src='./img/player_pfp.png'>"
                                         "<a class='endTableN' href='./social.html?p=profile&q=%s' target='_blank' style='color:%s'>%s</a></td>", name, color, shown_name);
            } else {
                char value[64];
                if (!strcmp(stats[i], "dmg") && row && row->dmg >= 0) snprintf(value, sizeof(value), "%d", (int) row->dmg); // endForm.dmg
                else hud_modes_end_value(player, hud_modes_end_entry(entry->uid), stats[i], value, sizeof(value));
                if (player_names_extras_hidden(player == client->me)) value[0] = 0; // Hide player names: the others' stats
                len += (size_t) snprintf(html + len, size - len, "<td style='position: relative;'>%s</td>", value);
            }
        }
        len += (size_t) snprintf(html + len, size - len, "<td style='width: 1px;white-space: nowrap;padding-right: 0px;padding-left: 0px;'></td></tr>");
    }

    return 1;
}

// endGame's #endTable with the mode's endStats (rows in the server's endSort order,
// your row white, your team 60% white, the enemy COLORS.teams[1]) and teamTotal0/1.
static void hud_modes_end_screen(const Client *client, const GameModeConfig *config) {
    if (!g_modes.end_valid || g_modes.end_applied || !config) return;

    const HtmlElem mid = html_dom_by_id("endMidHolder");
    char display[32];
    if (!mid || html_dom_get_style(mid, "display", display, sizeof(display)) < 0 || strcmp(display, "inline-block")) return;

    const HtmlElem table = html_dom_by_id("endTable");
    if (!table) return;
    g_modes.end_applied = 1;

    if (config->teams) {
        const int winner = g_modes.end.winner_team;
        char name[2][64], html[512];
        for (int t = 0; t < 2; t++) html_escape(mode_team_name(&client->game, t + 1), name[t], sizeof(name[t]));

        snprintf(html, sizeof(html), "%s<span class=\"teamTotalN0\">%s</span><span class=\"teamTotalScore\">%d</span>",
                 winner == 1 ? "<div class='material-icons teamWin0'>emoji_events</div>" : "", name[0], g_modes.end.team_scores[0]);
        HtmlElem el = html_dom_by_id("teamTotal0");
        if (el) {
            html_dom_set_html(el, html);
            html_dom_set_display(el, "inline-block");
        }
        snprintf(html, sizeof(html), "<span class=\"teamTotalScore\">%d</span><span class=\"teamTotalN1\">%s</span>%s",
                 g_modes.end.team_scores[1], name[1], winner == 2 ? "<div class='material-icons teamWin1'>emoji_events</div>" : "");
        el = html_dom_by_id("teamTotal1");
        if (el) {
            html_dom_set_html(el, html);
            html_dom_set_display(el, "inline-block");
        }
    }

    char *html = malloc(16384);
    if (!html) return;
    hud_modes_end_table_html(client, html, 16384);
    html_dom_set_html(table, html);
    free(html);
}

// ------------------------------------------------------------------ tick

void hud_modes_tick(Client *client) {
    mode_world_tick(client); // zones, flags, tags in the world
    if (!client || !html_ui_available() || !html_ui_is_ready() || !client->game.ready) return;

    const GameModeConfig *config = hud_mode_config(client);
    hud_modes_scores(client, config);
    hud_modes_message(client, config);
    hud_modes_lives(config);
    hud_modes_team_name(client, config);
    hud_modes_blocker(client);
    hud_modes_end_screen(client, config);
}

// ------------------------------------------------------------------ queries

int hud_modes_flag_count(void) {
    return g_modes.flag_count;
}

int hud_modes_flag(const int index, vec3 *position, int *team, int *carrier_uid) {
    if (index < 0 || index >= g_modes.flag_count) return 0;
    const NetModeFlag *flag = &g_modes.flags[index];
    if (!flag->active) return 0; // not in play: the caller shows the flag at its map position
    if (position) *position = (vec3) {flag->position[0], flag->position[1], flag->position[2]};
    if (team) *team = flag->team;
    if (carrier_uid) *carrier_uid = flag->carrier_uid;
    return 1;
}

// v7: activeObjective is the objective's uid
int hud_modes_active_objective(const Client *client, vec3 *center, vec3 *half) {
    const GameMode *mode = client->game.mode;
    if (!mode || !mode->config.objective) return 0;
    const ModeObjective *objective = mode_objective_by_uid(&client->game, mode->active_objective);
    if (!objective) return 0;
    if (center) *center = objective->position;
    if (half) *half = objective->half;
    return 1;
}

int hud_modes_spawnable(const int index, vec3 *position, int *team) {
    if (index < 0 || index >= MODE_MAX_SPAWNABLES || !g_modes.tags[index].active) return 0;
    if (position) *position = g_modes.tags[index].position;
    if (team) *team = g_modes.tags[index].team;
    return 1;
}

int hud_modes_spawnable_type(const int index, int *data_c) {
    if (index < 0 || index >= MODE_MAX_SPAWNABLES || !g_modes.tags[index].active) return -1;
    if (data_c) *data_c = g_modes.tags[index].data_c;
    return g_modes.tags[index].type;
}

const HudModesV7 *hud_modes_v7(void) {
    static HudModesV7 state;
    for (int t = 0; t < MODE_TEAMS; t++) {
        state.team_scores[t] = g_modes.team_scores_v7[t];
        state.team_round_wins[t] = g_modes.team_round_wins[t];
    }
    state.team_scores[1] = g_modes.team_scores[0];
    state.team_scores[2] = g_modes.team_scores[1];
    state.rounds_left = g_modes.rounds_left;
    state.lockout_ms = g_modes.lockout_ms;
    state.dog_tags = g_modes.dog_tags;
    state.popup_title = g_modes.popup_title;
    state.popup_text = g_modes.popup_text;
    state.popup_seq = g_modes.popup_seq;
    return &state;
}

const char *hud_modes_flag_info(const int team) {
    return team >= 1 && team <= 2 ? g_modes.flag_info[team - 1] : "";
}

// --render-scenario state.modeHud (ui_scenario.c, every frame): the mode state a server would
// have sent, pinned. team: the local player's; teamScores [t1, t2]; flagInfo [t1, t2] (#tScoreM); flagCarrier [t1, t2]:
// who carries each team's flag ("me", "enemy", null at base); gameMessage: the gmsg text.
void hud_modes_scenario(Client *client, const cJSON *hud) {
    if (!cJSON_IsObject(hud)) return;
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(hud, "team"); // the local player's team (the "you" icon)
    if (cJSON_IsNumber(v) && client->me && client->me->team != v->valueint) {
        client->me->team = v->valueint;
        hud_ui_render_team(client); // the card's team name (the spawn already rendered the sandbox's)
    }
    v = cJSON_GetObjectItemCaseSensitive(hud, "teamScores");
    for (int t = 0; t < 2 && cJSON_IsArray(v); t++) {
        const cJSON *n = cJSON_GetArrayItem(v, t);
        if (cJSON_IsNumber(n)) g_modes.team_scores[t] = n->valueint;
        g_modes.score_seen[t] = cJSON_IsNumber(n); // null: the team has not scored
    }
    v = cJSON_GetObjectItemCaseSensitive(hud, "flagInfo");
    for (int t = 0; t < 2 && cJSON_IsArray(v); t++) {
        const cJSON *s = cJSON_GetArrayItem(v, t);
        if (cJSON_IsString(s)) snprintf(g_modes.flag_info[t], NET_MODE_TEXT_SIZE, "%s", s->valuestring);
    }
    v = cJSON_GetObjectItemCaseSensitive(hud, "flagCarrier");
    for (int f = 0; f < g_modes.flag_count && cJSON_IsArray(v); f++) {
        const int team = g_modes.flags[f].team;
        if (team < 1 || team > 2) continue;
        const cJSON *who = cJSON_GetArrayItem(v, team - 1);
        g_modes.flags[f].carrier_uid = cJSON_IsString(who) && !strcmp(who->valuestring, "me") && client->me ? client->me->uid
                                     : cJSON_IsString(who) ? 0x7ffffff0 : -1;
    }
    v = cJSON_GetObjectItemCaseSensitive(hud, "gameMessage");
    g_scenario_message_set = cJSON_IsString(v); // over the sandbox's own gmsg packets
    if (g_scenario_message_set) snprintf(g_scenario_message, sizeof(g_scenario_message), "%s", v->valuestring);
}
