// Krunker v10.0.3 Stalker HUD on the v10 page (client/include/stalker_client.h, doc/v10/stalker.md).
// The markup is the page's (ui/index.html: #stalkerControlHolder, #stalkerLivingInfoHolder, #hiddenMsg,
// #zPerksHolder; ui/css/main.css); the strings are v10's literals (not in its translation table).
//
//   becoming the stalker ("tm" with isStalker, iîïíïíi deob-10.0.3.js:258386-258392): resetAim, the
//       control holder shown, the Living's info hidden, the leap item in #zPerksHolder and the popup
//       "You are the stalker" / "Hunt your prey!" (textPopup iíìíiìì :259252). KrunkNative watches
//       isStalker turn on for the local player instead of hooking the team packet: same frame, same effect.
//   STALKER_PICKED (iïiîiïí :255146): "You are the Living" / "Hunt the stalker and survive the round!".
//   spawn (:258175-258176): #stalkerLivingInfoHolder while our team option is "Living".
//   every frame (:261068-261080): while we are the stalker, the leap item: plain while the leap is ready,
//       .stalkerLeapTimer with opacity 1 - left / 3000 and the seconds left ("2.4s") while it recharges.
//   isHidden flips (procInputs :191773-191777): #hiddenMsg ("You are hidden") for the local player.
//   updateKeybindUI (:244244-244267): #stalkerLeap [aim], #stalkerStickToWall [jump], #stalkerTaunt0..5.
//   spectating (toggleGameUI :180751-180755) and a new game (:257095-257096, :256351): both holders and
//       #hiddenMsg hidden.
#include <stalker_client.h>
#include <stalker.h>
#include <ui_logic.h>
#include <html_dom.h>
#include <html_ui.h>
#include <net_ext.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define LEAP_ITEM "<div class='zPerkItem stalkerLeapItem' style='background-image:url(&quot;./img/stalker_leap.png&quot;)'><span></span></div>"

static struct {
    int was_stalker;  // isStalker of the local player last frame (the "tm" transition)
    int control;      // what #stalkerControlHolder / #stalkerLivingInfoHolder / #hiddenMsg show: -1 unknown
    int living;
    int hidden;
    int epoch;        // hud_ui.c's layout epoch the three above were written for
    int leap;         // #zPerksHolder holds our leap item: 0 none, 1 ready, 2 recharging
    char leap_text[16];
    int keys[2 + STALKER_TAUNT_COUNT]; // the codes the key labels were written for (-2 = rewrite)
} g_hs = {.control = -1, .living = -1, .hidden = -1, .epoch = -1};

static void set_display(const char *id, const int shown) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, shown ? "block" : "none");
}

static const char *team_option(const Client *client, const Player *player) {
    const GameMode *mode = client->game.mode;
    if (!mode || !mode->config.stalker_v10 || player->team < 1 || player->team > 2) return NULL;
    return mode->config.team_options[player->team - 1];
}

// players.resetAim (deob-10.0.3.js:195475-195481): RENDER.zoom(1), the scope and sight reticles at 0
static void reset_aim(Client *client) {
    client->camera.zoom = 1.0f;
    const HtmlElem scope = html_dom_by_id("aimRecticle"), dot = html_dom_by_id("aimDot");
    if (scope) html_dom_set_style(scope, "opacity", "0");
    if (dot) html_dom_set_style(dot, "opacity", "0");
    client->hud_html.scoped = 0;
    client->hud_html.aim_dot = 0;
}

// "[" + UTILS.getKeyName(val) + "]" into #id when the bind changed
static void key_label(const int slot, const char *id, const int code) {
    if (g_hs.keys[slot] == code) return;
    g_hs.keys[slot] = code;
    char name[32], text[40];
    snprintf(text, sizeof(text), "[%s]", ui_controls_key_name(code, name, sizeof(name)));
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_text(el, text);
}

static void key_labels(void) {
    key_label(0, "stalkerLeap", ui_controls_bind(UI_CONTROL_AIM));
    key_label(1, "stalkerStickToWall", ui_controls_bind(UI_CONTROL_JUMP));
    for (int i = 0; i < STALKER_TAUNT_COUNT; i++) {
        char id[16];
        snprintf(id, sizeof(id), "stalkerTaunt%d", i);
        key_label(2 + i, id, ui_controls_v7_val("stalkerTaunt", i));
    }
}

// the leap item while we are the stalker (deob-10.0.3.js:261069-261080); 0 = take ours out
static void leap_item(const Client *client, const int stalker) {
    if (!stalker && !g_hs.leap) return; // nothing of ours there
    const HtmlElem holder = html_dom_by_id("zPerksHolder");
    if (!holder) return;
    HtmlElem item = html_dom_query(holder, ".stalkerLeapItem");
    if (!stalker) {
        if (item && g_hs.leap) html_dom_set_html(holder, "");
        g_hs.leap = 0;
        return;
    }
    const Player *me = client->me;
    const float since = (float) client->game.now - me->last_stalker_leap;
    const float left = me->last_stalker_leap > 0.0f && since < STALKER_LEAP_COOLDOWN ? STALKER_LEAP_COOLDOWN - since : 0.0f;
    const int state = left > 0.0f ? 2 : 1;
    // the holder is shared with the zombie perks and emptied on death / a new game: rebuilt when ours is gone
    if (!item || state != g_hs.leap) {
        html_dom_set_html(holder, state == 2 ? "<div class='zPerkItem stalkerLeapItem stalkerLeapTimer' style='background-image:url(&quot;./img/"
                                               "stalker_leap.png&quot;)'><span style='position: absolute; top: 0; left: 0; color: #fff;'>"
                                               "</span></div>" : LEAP_ITEM);
        g_hs.leap = state;
        g_hs.leap_text[0] = 0;
        item = html_dom_query(holder, ".stalkerLeapItem");
    }
    if (state != 2 || !item) return;
    // opacity: 1 - left / 3000, the text (Math.round(left / 100) / 10).toFixed(1) + "s"
    char value[16], text[16];
    snprintf(value, sizeof(value), "%.3f", 1.0f - left / STALKER_LEAP_COOLDOWN);
    html_dom_set_style(item, "opacity", value);
    snprintf(text, sizeof(text), "%.1fs", (double) roundf(left * 10.0f) / 10.0);
    if (strcmp(text, g_hs.leap_text) != 0) {
        snprintf(g_hs.leap_text, sizeof(g_hs.leap_text), "%s", text);
        const HtmlElem span = html_dom_query(item, "span");
        if (span) html_dom_set_text(span, text);
    }
}

void hud_stalker_tick(Client *client, const int visible, const int layout_epoch) {
    if (!client || !html_ui_available() || !html_ui_is_ready()) return;
    const Player *me = client->me;
    const int alive = me && me->active;
    const int stalker = alive && player_is_stalker(&client->game, me);
    const char *option = alive ? team_option(client, me) : NULL;
    const int living = option && !strcmp(option, "Living");

    // the local player just became the stalker: resetAim and the popup (the holder follows below)
    if (stalker && !g_hs.was_stalker) {
        reset_aim(client);
        hud_popups_text_popup("You are the stalker", "Hunt your prey!", 0, 0);
    }
    g_hs.was_stalker = stalker;

    if (layout_epoch != g_hs.epoch) {
        g_hs.epoch = layout_epoch;
        g_hs.control = g_hs.living = g_hs.hidden = -1;
    }
    const int show_control = visible && stalker, show_living = visible && living && !stalker;
    const int hidden = visible && me && me->is_hidden;
    if (show_control != g_hs.control) {
        set_display("stalkerControlHolder", show_control);
        for (int i = 0; show_control && i < 2 + STALKER_TAUNT_COUNT; i++) g_hs.keys[i] = -2; // write every label
        g_hs.control = show_control;
    }
    if (show_living != g_hs.living) {
        set_display("stalkerLivingInfoHolder", show_living);
        g_hs.living = show_living;
    }
    if (hidden != g_hs.hidden) {
        set_display("hiddenMsg", hidden);
        g_hs.hidden = hidden;
    }
    if (show_control) key_labels();
    leap_item(client, stalker);
}

void hud_stalker_on_picked(Client *client) {
    (void) client;
    hud_popups_text_popup("You are the Living", "Hunt the stalker and survive the round!", 0, 0);
}

int hud_stalker_taunt(Client *client, const int index) {
    if (!client || !client->me || index < 0 || index >= STALKER_TAUNT_COUNT || !player_is_stalker(&client->game, client->me)) return 0;
    const NetStalkerTauntPacket body = {.index = (uint8_t) index};
    uint8_t buffer[NET_EXT_HEADER_SIZE + sizeof(body)];
    const uint16_t n = net_ext_pack(buffer, sizeof(buffer), NET_EXT_STALKER_TAUNT, &body, sizeof(body));
    if (n) client_net_send(client, NET_PACKET_EXT, buffer, n);
    return n != 0;
}
