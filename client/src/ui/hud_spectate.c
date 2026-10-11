// Krunker v3.1.1 spectator HUD (#spectateUI in ui/index.html, styled by main.css) over the
// DOM-ops API. Ported from the unpacked v3.1.1 client:
//   index.js  cw()      #specNames / #specStats (followed player's class icon, level,
//                       name, health bar #spectHPB) / #specSUS
//             fF(), fG() #specTeam0 / #specTeam1 player rows (.specPlayerHolder0/1: class
//                       icon, flag, name, slot number for the 1-9 keys, eye on the target)
//             fD()      #specTimer = the round clock;  fO()/106.js #specGMessage = #gameMessage
//             93.js     #specGameInfo = mode name;  spec0/spec1 team scores (team modes only)
//   103.js    UI.toggleGameUI / toggleControlUI with window.Bf5: #spectateUI shown, the
//             player HUD holders hidden, #topRight + chat kept
//   91.js     fakeCross: the small four-bar crosshair drawn while following in first person
// hud_ui.c owns the player HUD and samples nothing while we have no player, so this file
// keeps the holders it would otherwise leave in the wrong state in line every frame
// (inline style compared first, written only on a difference).
#include <spectate.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <render_settings.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define SPEC_SLOT_MAX 64

typedef struct {
    int uid;            // -1: free slot (fE[i].player == null)
    int team;           // fE[i].team
    HtmlElem elm;       // .specPlayerHolder<column>
    HtmlElem icon, flag, eye, kpd;
    int class_index;
    int silhouette;     // -1 unknown
    int eye_shown;      // -1 unknown
} SpecSlot;

static struct {
    int shown;                  // #spectateUI shown (toggleControlUI(true) while spectating), -1 unknown
    int was_active;
    SpecSlot slots[SPEC_SLOT_MAX];
    int slot_count;
    char stats_key[256];        // what #specStats / #specNames currently show
    int dirty;                  // cw() requested
    char timer[16];
    char info[64];
    char message[256];
    char person[16];
    char teams_key[160];
    char recticle[16];
    int cross;                  // fake crosshair shown, -1 unknown
} g_hs = {.shown = -1, .cross = -1};

/* ------------------------------------------------------------ helpers */

static void hs_set_display(const char *id, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return;
    char current[32];
    if (html_dom_get_style(el, "display", current, sizeof(current)) >= 0 && !strcmp(current, value)) return;
    html_dom_set_display(el, value);
}

static void hs_set_style(const char *id, const char *prop, const char *value) {
    const HtmlElem el = html_dom_by_id(id);
    if (!el) return;
    char current[64];
    if (html_dom_get_style(el, prop, current, sizeof(current)) >= 0 && !strcmp(current, value)) return;
    html_dom_set_style(el, prop, value);
}

static void hs_set_text(const char *id, char *cache, const size_t size, const char *text) {
    if (!strcmp(cache, text)) return;
    snprintf(cache, size, "%s", text);
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_text(el, text);
}

static void hs_append_escaped(char *out, const size_t size, size_t *len, const char *text) {
    for (const char *c = text ? text : ""; *c && *len + 8 < size; c++) {
        const char *rep = *c == '&' ? "&amp;" : *c == '<' ? "&lt;" : *c == '>' ? "&gt;" : *c == '"' ? "&quot;" : *c == '\'' ? "&#39;" : NULL;
        if (rep) *len += (size_t) snprintf(out + *len, size - *len, "%s", rep);
        else out[(*len)++] = *c;
    }
    out[*len] = 0;
}

static const char *hs_name(const Player *player) {
    return player->name[0] ? player->name : "Guest";
}

/* ------------------------------------------------------------ visibility */

// UI.toggleGameUI(true) + toggleControlUI(true) with window.Bf5 (j = 1, k = j && !Bf5 = 0).
static void hs_apply_shown(void) {
    hs_set_display("spectateUI", "block");
    hs_set_display("inGameUI", "block");
    hs_set_display("propInstruct", "none");
    hs_set_display("gameMessage", "none");
    hs_set_display("topLeftHolder", "none");
    hs_set_display("topRight", "block");
    hs_set_display("reloadMsg", "none");
    hs_set_display("bottomLeftHolder", "none");
    hs_set_display("weapDisplay", "none");
    hs_set_display("bottomRight", "none");
    hs_set_display("killCardHolder", "none");
    hs_set_display("overlay", "block");
    hs_set_display("vignette", "block");
    hs_set_display("igXPBar", "none");
    hs_set_display("aimRecticle", "block");
    hs_set_display("chatUI", "block");
    hs_set_display("interactProg", "none");
    hs_set_display("interactMsg", "none");
    hs_set_display("knCrosshair", "none"); // hud_ui.c's player crosshair (overlay canvas: !Bf5 only)
    hs_set_display("knHitmarker", "none");
    hs_set_style("speakerDisplay", "right", "380px");
    hs_set_style("chatHolder", "bottom", "20px");
    hs_set_style("voiceDisplay", "bottom", "18px");
    const HtmlElem base = html_dom_by_id("uiBase");
    if (base && !html_dom_has_class(base, "onGame")) html_dom_set_class(base, "onGame", 1);
}

// toggleControlUI(false) (menu / end screen over the spectator view); the chat stays.
static void hs_apply_hidden(const int still_spectating) {
    hs_set_display("spectateUI", "none");
    hs_set_display("inGameUI", "none");
    hs_set_display("overlay", "none");
    hs_set_display("vignette", "none");
    hs_set_display("aimRecticle", "none");
    if (still_spectating) hs_set_display("chatUI", "block");
}

/* ------------------------------------------------------------ player rows (fF / fG) */

static void hs_slot_clear(SpecSlot *slot) {
    if (slot->elm) html_dom_remove(slot->elm);
    memset(slot, 0, sizeof(*slot));
    slot->uid = -1;
}

void hud_spectate_reset(void) {
    // fc(): specTeam1.innerHTML = specTeam0.innerHTML = ""; fE.length = 0
    for (int i = 0; i < g_hs.slot_count; i++) g_hs.slots[i].elm = 0;
    g_hs.slot_count = 0;
    HtmlElem el;
    if ((el = html_dom_by_id("specTeam0"))) html_dom_set_html(el, "");
    if ((el = html_dom_by_id("specTeam1"))) html_dom_set_html(el, "");
    g_hs.stats_key[0] = 0;
    g_hs.timer[0] = g_hs.info[0] = g_hs.message[0] = g_hs.person[0] = g_hs.teams_key[0] = g_hs.recticle[0] = 0;
    g_hs.cross = -1;
    g_hs.dirty = 1;
}

int hud_spectate_slot_uid(const int index) {
    return index >= 0 && index < g_hs.slot_count ? g_hs.slots[index].uid : -1;
}

static int hs_slot_of(const int uid) {
    for (int i = 0; i < g_hs.slot_count; i++) {
        if (g_hs.slots[i].uid == uid) return i;
    }
    return -1;
}

// fF(player, team, column): first free slot (or a new one), row markup into #specTeam<column>.
static void hs_slot_add(const Player *player, const int column) {
    int index = -1;
    for (int i = 0; i < g_hs.slot_count; i++) {
        if (g_hs.slots[i].uid < 0) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        if (g_hs.slot_count >= SPEC_SLOT_MAX) return;
        index = g_hs.slot_count++;
        g_hs.slots[index].elm = 0;
    }

    SpecSlot *slot = &g_hs.slots[index];
    hs_slot_clear(slot);
    slot->uid = player->uid;
    slot->team = player->team;
    slot->class_index = player->class_index;
    slot->silhouette = -1;
    slot->eye_shown = -1;

    char html[1024];
    size_t len = (size_t) snprintf(html, sizeof(html),
        "<img src='textures/classes/icon_%d.png' class='specPlayerIcon%s' id='specPI_%d'>"
        "<div class='specPlayerFlag' id='sf_-1'></div><div class='specPlayerName'>",
        player->class_index, player->active ? "" : " silhouette", player->class_index);
    hs_append_escaped(html, sizeof(html), &len, hs_name(player));
    char num[8] = "";
    if (index < 9) snprintf(num, sizeof(num), "%d", index + 1);
    snprintf(html + len, sizeof(html) - len,
        "</div><div class='specNum'>%s</div><span class='material-icons' style='color:rgba(255,255,255,0.7);"
        "font-size:20px;text-shadow:none;margin-top:-47px;vertical-align: middle;' id='specH_%d'>visibility</span>"
        "<span class='material-icons' id='specKPD'>search</span>", num, player->uid);

    char cls[32];
    snprintf(cls, sizeof(cls), "specPlayerHolder%d", column);
    const HtmlElem holder = html_dom_create("div", NULL);
    const HtmlElem team = html_dom_by_id(column ? "specTeam1" : "specTeam0");
    if (!holder || !team) return;
    html_dom_set_attr(holder, "class", cls);
    html_dom_set_html(holder, html);
    html_dom_append(team, holder);

    slot->elm = holder;
    slot->icon = html_dom_query(holder, ".specPlayerIcon");
    slot->flag = html_dom_query(holder, ".specPlayerFlag");
    slot->eye = html_dom_query(holder, "span.material-icons");
    slot->kpd = html_dom_query(holder, "#specKPD");
}

// fG(): rows for new players, drop left/re-teamed ones, refresh icon / silhouette / eye.
static void hs_update_slots(const Client *client) {
    const int teams = client->game.mode && client->game.mode->config.teams;
    const int target = spectate_target_uid();

    for (size_t i = 0; i < client->game.player_count; i++) {
        const Player *player = client->game.players[i];
        if (!player || player == client->me) continue;
        if (hs_slot_of(player->uid) < 0) hs_slot_add(player, teams ? (player->team - 1 == 1) : (int) (i & 1));
    }

    for (int i = 0; i < g_hs.slot_count; i++) {
        SpecSlot *slot = &g_hs.slots[i];
        if (slot->uid < 0) continue;

        const Player *player = NULL;
        for (size_t k = 0; k < client->game.player_count && !player; k++) {
            if (client->game.players[k] && client->game.players[k]->uid == slot->uid) player = client->game.players[k];
        }

        // g0() (left) / a team change: the slot is freed for the next player.
        if (!player || player->team != slot->team) {
            hs_slot_clear(slot);
            continue;
        }

        if (slot->icon && player->class_index != slot->class_index) {
            char src[48], id[24];
            snprintf(src, sizeof(src), "textures/classes/icon_%d.png", player->class_index);
            snprintf(id, sizeof(id), "specPI_%d", player->class_index);
            html_dom_set_attr(slot->icon, "src", src);
            html_dom_set_attr(slot->icon, "id", id);
            slot->class_index = player->class_index;
        }

        const int silhouette = !player->active;
        if (slot->icon && silhouette != slot->silhouette) {
            html_dom_set_class(slot->icon, "silhouette", silhouette);
            slot->silhouette = silhouette;
        }

        const int eye = target >= 0 && target == player->uid;
        if (slot->eye && eye != slot->eye_shown) {
            html_dom_set_display(slot->eye, eye ? "inline-block" : "none");
            if (slot->kpd) html_dom_set_display(slot->kpd, "none"); // no K.P.D reports in KrunkNative
            slot->eye_shown = eye;
        }
    }
}

/* ------------------------------------------------------------ followed player (cw) */

void hud_spectate_refresh(void) {
    g_hs.dirty = 1;
}

static void hs_number(const double value, char *out, const size_t size) {
    if (value == floor(value)) snprintf(out, size, "%.0f", value);
    else snprintf(out, size, "%.6g", value);
}

static void hs_update_stats(const Client *client) {
    const int uid = spectate_target_uid();
    const Player *target = uid >= 0 ? client_find_player((Client *) client, uid) : NULL;

    char key[256];
    if (target) {
        const int hp = (int) ceilf(target->health > 0.0f ? target->health : 0.0f);
        const int mhp = target->max_health > 0 ? target->max_health : 100;
        snprintf(key, sizeof(key), "%d|%d|%d|%d|%s", target->uid, target->class_index, hp, mhp, hs_name(target));
    } else {
        snprintf(key, sizeof(key), "-");
    }

    if (!g_hs.dirty && !strcmp(key, g_hs.stats_key)) return;
    g_hs.dirty = 0;
    snprintf(g_hs.stats_key, sizeof(g_hs.stats_key), "%s", key);

    if (!target) {
        hs_set_display("specNames", "block");
        hs_set_display("specStats", "none");
        hs_set_display("specSUS", "none");
        return;
    }

    const int hp = (int) ceilf(target->health > 0.0f ? target->health : 0.0f);
    const int mhp = target->max_health > 0 ? target->max_health : 100;
    const double pct = (double) hp / mhp * 100.0;
    const ClientRenderSettings *rs = client_render_settings();
    const char *color = pct <= 30.0 ? rs->hud_health_low : rs->hud_health_high;
    char width[32];
    hs_number(pct, width, sizeof(width));

    // Guests: level 0, challenge level 1 (getChallCol(1) = white), no flag, no clan, not featured.
    char html[1536];
    size_t len = (size_t) snprintf(html, sizeof(html),
        "<img class='spectPIcon' src='textures/classes/icon_%d.png'/><span id='f_-1' class='specFlag'></span>"
        "<div style='display:inline-block;width:200px;vertical-align:top'><div class='specLevelBox' id='lvl_0_1' "
        "style='color:white;'>0</div><span class='specPHead' id='s_%d'>", target->class_index, target->uid);
    hs_append_escaped(html, sizeof(html), &len, hs_name(target));
    snprintf(html + len, sizeof(html) - len,
        "</span><i class='material-icons specVerified' style='display:none'>check_circle</i><div id='spectHPB'>"
        "<span id='spectHPV'>%d | %d</span><div id='spectHPBI' style='width:%s%%;background-color:%s'></div></div></div>",
        hp, mhp, width, color);

    const HtmlElem stats = html_dom_by_id("specStats");
    if (stats) html_dom_set_html(stats, html);
    hs_set_display("specNames", "none");
    hs_set_display("specSUS", "none");
    hs_set_display("specStats", "inline-block");
}

/* ------------------------------------------------------------ the rest of #spectateHUD */

static void hs_update_info(const Client *client) {
    // fD(): #specTimer mirrors #timerVal ("MM:SS")
    const int seconds = client->net_round_seconds >= 0 ? client->net_round_seconds
                                                       : (client->game.config.game_time > 0 ? client->game.config.game_time * 60 : 0);
    char timer[16];
    snprintf(timer, sizeof(timer), "%02d:%02d", seconds / 60, seconds % 60);
    hs_set_text("specTimer", g_hs.timer, sizeof(g_hs.timer), timer);

    const char *mode = client->game.mode && client->game.mode->config.name ? client->game.mode->config.name : "Free for All";
    hs_set_text("specGameInfo", g_hs.info, sizeof(g_hs.info), mode);

    // 106.js: specGMessage follows gameMessage
    char message[256] = "";
    const HtmlElem game_message = html_dom_by_id("gameMessage");
    if (game_message && html_dom_get_text(game_message, message, sizeof(message)) < 0) message[0] = 0;
    hs_set_text("specGMessage", g_hs.message, sizeof(g_hs.message), message);

    hs_set_text("specIns", g_hs.person, sizeof(g_hs.person), spectate_person_label());

    // spec0/spec1: team names and scores in team modes only; no objective cams (specContr child 3).
    const int teams = client->game.mode && client->game.mode->config.teams;
    char key[160];
    if (teams) {
        snprintf(key, sizeof(key), "%s|%s|%d|%d", mode_team_name(&client->game, 1), mode_team_name(&client->game, 2),
                 mode_team_score(&client->game, 1), mode_team_score(&client->game, 2));
    } else {
        snprintf(key, sizeof(key), "-");
    }
    if (strcmp(key, g_hs.teams_key) != 0) {
        snprintf(g_hs.teams_key, sizeof(g_hs.teams_key), "%s", key);
        hs_set_display("spec0", teams ? "inline-block" : "none");
        hs_set_display("spec1", teams ? "inline-block" : "none");
        if (teams) {
            char score[16];
            HtmlElem el;
            if ((el = html_dom_by_id("specNameTm0"))) html_dom_set_text(el, mode_team_name(&client->game, 1));
            if ((el = html_dom_by_id("specNameTm1"))) html_dom_set_text(el, mode_team_name(&client->game, 2));
            snprintf(score, sizeof(score), "%d", mode_team_score(&client->game, 1));
            if ((el = html_dom_by_id("specScoreTm0"))) html_dom_set_text(el, score);
            snprintf(score, sizeof(score), "%d", mode_team_score(&client->game, 2));
            if ((el = html_dom_by_id("specScoreTm1"))) html_dom_set_text(el, score);
        }
        const HtmlElem contr = html_dom_by_id("specContr");
        HtmlElem objective = 0;
        if (contr) {
            HtmlElem items[8];
            if (html_dom_query_all(contr, ".specContrI", items, 8) >= 3) objective = items[2];
        }
        if (objective) html_dom_set_display(objective, "none");
        hs_set_display("specKPDContr", "none");
    }
}

// 91.js: fakeCross while following in first person (not over a full scope), and the
// followed player's scope (#aimRecticle opacity = 1 - jKFi4).
static void hs_update_view(const Client *client) {
    const int uid = spectate_target_uid();
    const Player *target = uid >= 0 && spectate_first_person() ? client_find_player((Client *) client, uid) : NULL;
    const int scoped = target && target->active && target->weapon && target->weapon->scope;
    const float opacity = scoped ? CLAMP(target->aim_val, 0.0f, 1.0f) : 0.0f;

    char value[16];
    snprintf(value, sizeof(value), "%.3g", opacity);
    if (strcmp(value, g_hs.recticle) != 0) {
        snprintf(g_hs.recticle, sizeof(g_hs.recticle), "%s", value);
        const HtmlElem el = html_dom_by_id("aimRecticle");
        if (el) html_dom_set_style(el, "opacity", value);
    }

    const int cross = target && target->active && opacity < 1.0f;
    if (cross == g_hs.cross) return;
    g_hs.cross = cross;

    HtmlElem el = html_dom_by_id("knSpecCross");
    if (!el && cross) {
        // ao(ctx, 2, 4, 1, 1, angle) x4: 3.8 x 7.6 bars 4 px off the centre, overlay (#uiBase) px.
        const HtmlElem ui = html_dom_by_id("spectateUI");
        if (!ui) return;
        static const char *const bars =
            "<div id='knSpecCross' style='position:absolute;left:50%;top:50%;width:0;height:0;pointer-events:none'>"
            "<div style='position:absolute;left:-1.9px;top:-13.5px;width:3.8px;height:7.6px;background-color:#fff;box-shadow:0 0 0 1px rgba(0,0,0,0.5)'></div>"
            "<div style='position:absolute;left:-1.9px;top:5.9px;width:3.8px;height:7.6px;background-color:#fff;box-shadow:0 0 0 1px rgba(0,0,0,0.5)'></div>"
            "<div style='position:absolute;left:-13.5px;top:-1.9px;width:7.6px;height:3.8px;background-color:#fff;box-shadow:0 0 0 1px rgba(0,0,0,0.5)'></div>"
            "<div style='position:absolute;left:5.9px;top:-1.9px;width:7.6px;height:3.8px;background-color:#fff;box-shadow:0 0 0 1px rgba(0,0,0,0.5)'></div>"
            "</div>";
        html_dom_insert_html(ui, HTML_INSERT_BEFORE_END, bars);
        el = html_dom_by_id("knSpecCross");
    }
    if (el) html_dom_set_display(el, cross ? "block" : "none");
}

/* ------------------------------------------------------------ tick */

void hud_spectate_tick(Client *client) {
    if (!html_ui_available() || !html_ui_is_ready()) return;

    const int active = spectate_active();
    const int end_screen = client->net_end_seconds >= 0;
    const int shown = active && !html_ui_is_menu_open() && !end_screen && client->game.ready;

    if (!active) {
        if (g_hs.was_active) {
            // Spectating ended (own spawn, INIT, disconnect): hud_ui.c takes the HUD back;
            // toggleGameUI(true) without Bf5 puts the chat back above the player HUD.
            const int playing = client->in_game && !html_ui_is_menu_open(); // entering the game (SPAWN pending or done)
            hs_set_display("spectateUI", "none");
            hs_set_display("knSpecCross", "none");
            if (!client->me) hs_set_display("chatUI", "none");
            hs_set_style("chatHolder", "bottom", playing ? "148px" : "20px");
            hs_set_style("voiceDisplay", "bottom", playing ? "145px" : "18px");
            g_hs.shown = -1;
        }
        g_hs.was_active = 0;
        return;
    }
    if (!g_hs.was_active) hud_spectate_reset();
    g_hs.was_active = 1;

    if (shown) hs_apply_shown(); // every frame: only differing inline styles are written
    else if (g_hs.shown != 0) hs_apply_hidden(1);
    g_hs.shown = shown;
    if (!shown) {
        hs_set_display("chatUI", "block");
        return;
    }

    hs_update_slots(client);
    hs_update_stats(client);
    hs_update_info(client);
    hs_update_view(client);
}
