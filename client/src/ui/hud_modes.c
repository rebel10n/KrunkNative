// Game mode HUD + client mirror of the mode packets (see client/include/hud_modes.h).
// Krunker v3.1.1 index.js: initGame (#teamScores markup, #livesCount, team names),
// updateTeamScore ("ts"), updateFlagInfo ("ufi"), updateGameMessage ("gmsg"),
// updateTeam ("tm"), updateLives ("lv"), actionFeed ("ac"), lockPlayer ("lock"),
// Game.updateFlags (the CTF flag messages) and the end screen's teamTotal0/1 + endStats.
#include <hud_modes.h>
#include <streamer.h>
#include <mode_world.h>
#include <html_ui.h>
#include <html_dom.h>
#include <ui_logic.h>
#include <stdio.h>
#include <string.h>

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
    vec3 position;
    unsigned char active:1;
} HudModesTag;

typedef struct {
    int mode_id;                     // -1: no round yet
    int team_scores[2];
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

    // what the DOM shows (rebuilt when these differ)
    char shown_scores[512];
    char shown_message[NET_GAME_MESSAGE_SIZE];
    char shown_lives[256];
    char shown_team[256];
    int shown_blocker;
    int chat_seq;
} HudModes;

static HudModes g_modes = {.mode_id = -1, .lives = -1};

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
        if (player->ammo) player->ammo[i] = (unsigned int) packet->ammo[i];
    }

    if (had_mesh) client_add_player_mesh(client, player, is_you);
}

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
        html_escape(streamer_name(actor->name, shown, sizeof(shown)), name, sizeof(name)); // fixStr(scrambleS(alias)) in streamer mode
        snprintf(left, sizeof(left), "<span style='color:%s'>%s</span>", local_team && local_team == actor->team ? MODES_COLOR_ALLY : MODES_COLOR_ENEMY, name);
    }

    right[0] = 0;
    if (other == client->me && other) {
        snprintf(right, sizeof(right), "<span style='color:#fff'>You</span>");
    } else if (other) {
        char shown[PLAYER_NAME_SIZE];
        html_escape(streamer_name(other->name, shown, sizeof(shown)), other_name, sizeof(other_name));
        snprintf(right, sizeof(right), "<span style='color:%s'>%s</span>", local_team && local_team == other->team ? MODES_COLOR_ALLY : MODES_COLOR_ENEMY, other_name);
    }

    char raw[NET_MODE_TEXT_SIZE];
    memcpy(raw, packet->text, sizeof(raw));
    raw[sizeof(raw) - 1] = 0;
    html_escape(raw, text, sizeof(text));

    // addChat(null, msg, 1): a plain game line
    snprintf(html, sizeof(html), "<div class='chatItem'><span class='chatMsg'>%s %s %s</span></div><br>", left, text, right);

    const HtmlElem list = html_dom_by_id("chatList");
    if (!list) return;
    char id[32];
    snprintf(id, sizeof(id), "chatMsg_m%d", g_modes.chat_seq++);
    const HtmlElem row = html_dom_create("div", id);
    html_dom_set_html(row, html);
    html_dom_append(list, row);
    html_dom_scroll_to_bottom(list);
}

static void hud_modes_apply_state(Client *client, const NetModeStatePacket *packet) {
    g_modes.mode_id = packet->mode_id;
    g_modes.team_scores[0] = packet->team_scores[0];
    g_modes.team_scores[1] = packet->team_scores[1];
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
            if (score.team >= 1 && score.team <= 2) {
                g_modes.team_scores[score.team - 1] = score.score;
                if (game->mode) game->mode->team_scores[score.team] = score.score;
            }
            return 1;
        }
        case NET_PACKET_FLAG: {
            MODES_READ(NetFlagPacket, flag);
            if (flag.index >= 0 && flag.index < MODE_MAX_FLAGS) {
                g_modes.flags[flag.index] = flag.flag;
                if (flag.index >= g_modes.flag_count) g_modes.flag_count = flag.index + 1;
                if (flag.flag.team >= 1 && flag.flag.team <= 2) {
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
            return 0;
    }
#undef MODES_READ
}

void hud_modes_reset(Client *client) {
    mode_world_unload(client);
    const int chat_seq = g_modes.chat_seq;
    memset(&g_modes, 0, sizeof(g_modes));
    g_modes.mode_id = -1;
    g_modes.lives = -1;
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

// initGame's #teamScores rows: CTF shows the score, the flag icon and the flag status;
// the other team modes a team colour square and the score. The local team's icon gets
// "you" (blue flag / white square).
static void hud_modes_scores(const Client *client, const GameModeConfig *config) {
    if (!config || !config->teams) {
        dom_set_display("teamScores", "none");
        g_modes.shown_scores[0] = 0;
        return;
    }

    const int team = hud_local_team(client);
    char html[MODES_HTML_SIZE];
    size_t len = 0;
    html[0] = 0;

    for (int t = 1; t <= 2; t++) {
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
    const char *text = g_modes.message;
    if (!strcmp(text, NET_GAME_MESSAGE_WAITING)) text = "waiting for players"; // i18n player.waiting

    if (config && config->flags && client->me) {
        int carrying = 0, enemy_has_ours = 0;
        for (int f = 0; f < g_modes.flag_count; f++) {
            const NetModeFlag *flag = &g_modes.flags[f];
            if (flag->carrier_uid < 0) continue;
            if (flag->team == client->me->team) enemy_has_ours = 1;
            if (flag->carrier_uid == client->me->uid) carrying = 1;
        }
        text = enemy_has_ours && carrying ? MODES_FLAG_MSG_RC : enemy_has_ours ? MODES_FLAG_MSG_E : carrying ? MODES_FLAG_MSG : text;
    }

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
    else if (!strcmp(stat, "time")) {                                     // endForm.time: timer || "DNF"
        const int tenths = s ? s[NET_MODE_STAT_TIME] - 1 : -1;            // UTILS.getTime(lastTimer, showMS): Race / Raid
        if (tenths < 0) snprintf(out, size, "DNF");
        else snprintf(out, size, "%02d:%02d.%d", tenths / 600 % 60, tenths / 10 % 60, tenths % 10);
    }
    else snprintf(out, size, "0");
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

    static const char *const default_stats[] = {"sid", "name", "score", "kills", "deaths", "reward", NULL};
    const char *const *stats = config->end_stats ? config->end_stats : default_stats;
    int stat_count = 0;
    while (stats[stat_count]) stat_count++;

    char *html = malloc(16384);
    if (!html) return;
    size_t len = 0;
    len += (size_t) snprintf(html + len, 16384 - len, "<tr><th>Rank</th>");
    for (int i = 1; i < stat_count; i++) len += (size_t) snprintf(html + len, 16384 - len, "<th>%s</th>", hud_modes_end_header(stats[i]));
    len += (size_t) snprintf(html + len, 16384 - len, "<th></th></tr>");

    const int local_team = hud_local_team(client);
    for (int r = 0; r < g_modes.end.count && r < NET_MODE_END_MAX && len < 15000; r++) {
        const NetModeEndEntry *entry = &g_modes.end.entries[r];
        const Player *player = client_find_player((Client *) client, entry->uid);
        if (!player) continue;

        // showEndTable: the link keeps the real name, the text is scrambleS(alias) in streamer mode
        char name[PLAYER_NAME_SIZE * 6], shown_name[PLAYER_NAME_SIZE * 6], shown[PLAYER_NAME_SIZE];
        html_escape(player->name, name, sizeof(name));
        html_escape(streamer_name(player->name, shown, sizeof(shown)), shown_name, sizeof(shown_name));
        const char *color = player == client->me ? "#fff" : local_team == player->team ? "rgba(255,255,255,0.6)" : MODES_TEAM_ENEMY;

        len += (size_t) snprintf(html + len, 16384 - len, "<tr style='%s'><td style='color:#fff'>%d.</td>",
                                 r + 1 >= g_modes.end.count ? "border-bottom:none" : "", r + 1);
        for (int i = 1; i < stat_count; i++) {
            if (!strcmp(stats[i], "name")) {
                len += (size_t) snprintf(html + len, 16384 - len, "<td><a class='endTableN' href='./social.html?p=profile&q=%s' target='_blank' style='color:%s'>%s</a></td>", name, color, shown_name);
            } else {
                char value[64];
                hud_modes_end_value(player, hud_modes_end_entry(entry->uid), stats[i], value, sizeof(value));
                len += (size_t) snprintf(html + len, 16384 - len, "<td>%s</td>", value);
            }
        }
        len += (size_t) snprintf(html + len, 16384 - len, "<td style='width: 1px;white-space: nowrap;padding-right: 0px;padding-left: 0px;'></td></tr>");
    }

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
    if (position) *position = (vec3) {flag->position[0], flag->position[1], flag->position[2]};
    if (team) *team = flag->team;
    if (carrier_uid) *carrier_uid = flag->carrier_uid;
    return 1;
}

int hud_modes_active_objective(const Client *client, vec3 *center, vec3 *half) {
    const GameMode *mode = client->game.mode;
    if (!mode || !mode->config.objective) return 0;
    if (mode->active_objective < 0 || mode->active_objective >= mode->objective_count) return 0;
    const ModeObjective *objective = &mode->objectives[mode->active_objective];
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
