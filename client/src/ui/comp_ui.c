// Krunker v7.1.4 comp menu (#mMenuHolComp): the menu of a comp lobby with the Test Mode setting
// (config.tstCmp, defC 1 on the comp host) — every ranked match (doc/v7/decisions.md "Ranked").
// index.js f1904 (the comp branch: class picker, "Krunker Competitive" NvN on <map>, the vs
// image while the lockout runs), f1892 (the lobby's two player lists, the class picker's locks
// and counts, Join / Leave Team), toggleTeam / selectClass (updSyncDat "t" / "c") and
// io_updateGameMessage's #compTimBox. The lobby table itself is shared/src/comp_lobby.c.
#include <lobby_ui.h>
#include <comp_lobby.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <weapons.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>

static int g_shown;     // #mMenuHolComp is up (f1904 took the comp branch)
static char g_msg[NET_GAME_MESSAGE_SIZE];

static void set_display(const char *id, const char *value) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void set_html(const char *id, const char *html) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_html(el, html);
}

static int comp_menu_on(const Client *client) {
    return client && client->game.config.comp && client->game.config.test_mode;
}

// Game.config.classes.indexOf(i) >= 0 (an empty list: every class)
static int class_offered(const Game *game, const int index) {
    if (game->config.class_list_count <= 0) return 1;
    for (int i = 0; i < game->config.class_list_count; i++) {
        if (game->config.class_list[i] == index) return 1;
    }
    return 0;
}

static const ClassConfig *class_at(const Game *game, const int index) {
    const size_t count = game->class_count ? game->class_count : CLASS_COUNT;
    const ClassConfig *classes = game->classes ? game->classes : rules_classes();
    return index >= 0 && (size_t) index < count ? &classes[index] : NULL;
}

// Game.weaponConfig[classConfig[c].loadout[0]].icon
static const char *class_weapon_icon(const Game *game, const int index) {
    const ClassConfig *c = class_at(game, index);
    if (!c || !c->loadout || !game->weapons) return NULL;
    const int w = c->loadout[0];
    return w >= 0 && (size_t) w < game->weapon_count && game->weapons[w] ? game->weapons[w]->icon : NULL;
}

static const char *map_name(const Game *game) {
    const int index = game->current_map_index;
    const cJSON *map = index >= 0 && (size_t) index < game->map_count ? game->maps[index] : NULL;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(map, "name"));
    return name ? name : "Unknown";
}

static void player_slot(StrBuf *sb, const Game *game, const CompLobbyEntry *e, const int left, const int my_team, const int slot) {
    const int hidden = e && my_team != e->team; // the other team's classes stay secret: "???"
    char weapon[160] = "";
    const char *icon = e && e->class_index >= 0 && !hidden ? class_weapon_icon(game, e->class_index) : NULL;
    if (icon) snprintf(weapon, sizeof(weapon), "<img src='./textures/weapons/%s.png' class='compPlWpIcn' />", icon);
    const char *verified = e && e->verified ? " <i class='material-icons' style='color:#40c4ff;margin-top:-8px;font-size:35px;vertical-align:middle;'>check_circle</i> "
                         : e && e->level ? "<div style='display:inline-block;width:10px'></div>" : "";
    const ClassConfig *cls = e && e->class_index >= 0 && !hidden ? class_at(game, e->class_index) : NULL;
    const char *class_name = cls ? cls->name : hidden ? "???" : "None";
    const int icon_index = e && !hidden && e->class_index >= 0 ? e->class_index : -1;
    char pfp[200];
    snprintf(pfp, sizeof(pfp), "<img class='compPLstIcn' id='compPfpI%d' style='border:3px solid #fff' src='./textures/classes/icon_%d.png' />", slot, icon_index);
    if (left) {
        sb_appendf(sb, "<div class='compPlLitL'><div class='compPlWepsL'>%s</div>%s<div class='compPLstSt'>%s", weapon, pfp, verified);
        if (e) sb_append_esc(sb, e->name[0] ? e->name : "Guest Player");
        else sb_append(sb, "Open Slot");
        sb_appendf(sb, "<div class='compPLstCl'>%s</div></div></div>", class_name);
    } else {
        sb_append(sb, "<div class='compPlLitR'><div class='compPLstSt compPLstStR' style='text-align:right'>");
        if (e) {
            sb_append_esc(sb, e->name[0] ? e->name : "Guest Player");
            sb_append(sb, verified);
        } else {
            sb_append(sb, "Open Slot");
        }
        sb_appendf(sb, "<div class='compPLstCl'>%s</div></div>%s<div class='compPlWepsR'>%s</div></div>", class_name, pfp, weapon);
    }
}

// f1892: the lists, the class picker's locks / counts, Join / Leave Team
void comp_ui_refresh(Client *client) {
    if (!g_shown || !comp_menu_on(client)) return;
    const Game *game = &client->game;
    const CompLobby *lobby = game->comp_lobby;
    const CompLobbyEntry *me = NULL;
    const CompLobbyEntry *active[COMP_LOBBY_MAX];
    int active_count = 0;
    for (int i = 0; lobby && i < lobby->count; i++) {
        const CompLobbyEntry *e = &lobby->entries[i];
        if (e->uid == client->net_client_id) me = e;
        if (e->active) active[active_count++] = e;
    }
    const int my_team = me ? me->team : 0;
    // the class picker: grey + lock when the team's slots of a class are taken
    const int limited = game->config.limit_classes;
    const size_t class_count = game->class_count ? game->class_count : CLASS_COUNT;
    for (size_t c = 0; c < class_count && c < GAME_CONFIG_CLASS_LIMITS; c++) {
        const int limit = game->config.class_limits[c];
        if (limited && !limit) continue;
        const int taken = limited ? comp_lobby_class_count(game, my_team, (int) c) : 0;
        const int full = limited && taken >= limit;
        char id[32];
        snprintf(id, sizeof(id), "cmpClassIc%zu", c);
        HtmlElem el = html_dom_by_id(id);
        if (el) html_dom_set_class(el, "cmpClassGr", full);
        snprintf(id, sizeof(id), "cmpClassLck%zu", c);
        if ((el = html_dom_by_id(id))) html_dom_set_display(el, full ? "inline-block" : "none");
        snprintf(id, sizeof(id), "cmpClassCnt%zu", c);
        if ((el = html_dom_by_id(id))) {
            html_dom_set_class(el, "cmpClassGr", full);
            char text[16];
            snprintf(text, sizeof(text), "%d/%d", taken, limit);
            html_dom_set_text(el, text);
        }
    }
    // the two lists: tmSize + 1 slots per team, team 1 left, team 2 right
    const int size = game->config.team_size + 1;
    StrBuf left = {0}, right = {0};
    sb_appendf(&left, "<div class='cmpTmHed'>%s</div>", game->config.team_names[1][0] ? game->config.team_names[1] : "Alpha");
    sb_appendf(&right, "<div class='cmpTmHed'>%s</div>", game->config.team_names[2][0] ? game->config.team_names[2] : "Bravo");
    for (int slot = 0; slot < size * 2; slot++) {
        const int team = slot < size ? 1 : 2;
        const CompLobbyEntry *e = NULL;
        for (int i = 0; i < active_count; i++) {
            if (active[i] && active[i]->team == team) {
                e = active[i];
                active[i] = NULL; // vA645.splice: each entry fills one slot
                break;
            }
        }
        player_slot(team == 1 ? &left : &right, game, e, team == 1, my_team, slot);
    }
    if (game->config.select_team) { // selTeam: Join / Leave Team under each list
        sb_appendf(&left, "<div class='compSwpTmB' style='background-color:#eb4034' onmouseenter='playTick()' onclick='playSelect();toggleTeam(%d)'>%s</div>",
                   my_team == 1 ? 0 : 1, my_team == 1 ? "Leave Team" : "Join Team");
        sb_appendf(&right, "<div class='compSwpTmB' style='background-color:#3489eb' onmouseenter='playTick()' onclick='playSelect();toggleTeam(%d)'>%s</div>",
                   my_team == 2 ? 0 : 2, my_team == 2 ? "Leave Team" : "Join Team");
    }
    char *l = sb_finish(&left), *r = sb_finish(&right);
    set_html("compPlListL", l);
    set_html("compPlListR", r);
    free(l);
    free(r);
}

// f1904(Game.isComp) at every round start
void comp_ui_round(Client *client) {
    set_display("mMenuHolDef", "none");
    set_display("mMenuHolComp", "none");
    set_display("vsHolder", "none");
    HtmlElem base = html_dom_by_id("uiBase");
    if (base) html_dom_set_class(base, "onCompMenu", 0);
    g_shown = 0;
    if (!comp_menu_on(client)) {
        set_display("mMenuHolDef", "block");
        return;
    }
    const Game *game = &client->game;
    menu_set_class_index(0, 1); // setClassIndex(0)
    StrBuf sb = {0};
    const int limited = game->config.limit_classes;
    const size_t class_count = game->class_count ? game->class_count : CLASS_COUNT;
    for (size_t c = 0; c < class_count && c < GAME_CONFIG_CLASS_LIMITS; c++) {
        if (!class_offered(game, (int) c)) continue;
        const int limit = game->config.class_limits[c];
        if (limited && !limit) continue;
        sb_appendf(&sb, "<div class='cmpClassLck' style='display:none;' id='cmpClassLck%zu'><span class='material-icons' style='font-size:70px;color:#fff;vertical-align:top;'>lock</span></div>"
                        "<div class='compClassPItm' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='SOUND.play(`reload_3`,0.09);playSelect(0.03);selectClass(%zu)'>"
                        "<img src='./textures/classes/icon_%zu.png' class='compClassPIcn' id='cmpClassIc%zu' />", c, c, c, c);
        if (limited) sb_appendf(&sb, "<span class='compClassPCnt' id='cmpClassCnt%zu'>0/%d</span>", c, limit);
        sb_append(&sb, "</div>");
    }
    char *html = sb_finish(&sb);
    set_html("compClassHold", html);
    free(html);
    g_shown = 1;
    comp_ui_refresh(client);
    if (game->mode && game->mode->lockout_timer > 0.0f) { // lockoutTimer: the vs image, no instructions
        set_display("vsHolder", "block");
        set_html("instructions", "");
        set_display("instructionHider", "none");
    }
    StrBuf sub = {0};
    sb_appendf(&sub, "%dv%d <span style='color:rgba(255,255,255,0.5);'>on ", game->config.team_size + 1, game->config.team_size + 1);
    sb_append_esc(&sub, map_name(game));
    sb_append(&sub, "</span>");
    html = sb_finish(&sub);
    set_html("cmpDatSub", html);
    free(html);
    set_display("aHider", "none");
    set_display("infoHider", "none");
    set_display("mMenuHolComp", "block");
    if (base) {
        html_dom_set_class(base, "onCompMenu", 1);
        html_dom_set_class(base, "onMenu", 0);
    }
}

// io_updateGameMessage: #compTimBox shows the game message ("Match Pending" until the first)
void comp_ui_message(const char *text) {
    if (!g_shown || !text || !strcmp(text, g_msg)) return;
    snprintf(g_msg, sizeof(g_msg), "%s", text);
    HtmlElem el = html_dom_by_id("compTimBox");
    if (el) html_dom_set_text(el, text);
}

// updSyncDat(key, value): NET_PACKET_COMP_SET to v7-protocol servers and offline
static void comp_send(Client *client, const char key, const int value) {
    if (!client || !client->game.config.comp || (!client->offline && client->net_server_minor < NET_PROTOCOL_MINOR_V7)) return;
    const NetCompSetPacket packet = {.key = (uint8_t) key, .value = value};
    client_net_send(client, NET_PACKET_COMP_SET, &packet, sizeof(packet));
}

// selectClass(i): updSyncDat("c", i)
void comp_ui_class_selected(Client *client, const int index) {
    comp_send(client, 'c', index);
}

// toggleTeam(team): updSyncDat("t", team), 0 leaves
int comp_ui_toggle_team(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    const int team = argc > 0 && args && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
    comp_send(user, 't', team);
    return 1;
}

int comp_ui_packet(Client *client, const void *payload, const uint16_t length) {
    NetCompLobbyPacket packet;
    if (length != sizeof(packet)) return 1;
    memcpy(&packet, payload, sizeof(packet));
    comp_lobby_apply(&client->game, &packet);
    comp_ui_refresh(client);
    return 1;
}
