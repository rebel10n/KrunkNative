// The page side of v10.0.3 S2C packets the 2026-10-24 follow-up pass ported (doc/v10/s2c-coverage.md; the packets come
// through client/src/net/s2c_events.c, the declarations are client/include/ui_logic.h's s2c_ui_*). Line numbers:
// deob-10.0.3.js (Krunker_v10.0.3/deob/game.deob.js).
//   chreq     a map script's KR charge: the #chargeModal card or the #chargeConfirm [1] / [2] prompt (iîiîiïi :257673),
//             answered with "chrsp" (iïííìíì :257700), closed unanswered after 15 s, at every init and on a disconnect
//             (iìîíïìì :257712, called :257109 / :261256)
//   warsClan  the Turf Wars clan holding the map (territorialWars.setClanInfo) and, once per game, 1 s after we entered,
//             textPopup("Turf Wars", "Territory controlled by <clan>") (enterGame :242096, iíìïïíì :255551). The
//             wars_banner map banners setClanInfo also builds (:215041-215088) are not drawn: KrunkNative's maps have no
//             wars_banner server nodes (doc/v10/ranked-impl.md)
//   edMp      the editor opens a map (iiíïìiì :259803): its objects' col / l false dropped, staged as mapToEdit, the
//             editor page opened (as F6 does, ui_mod_keys.c)
#include <ui_logic.h>
#include <client.h>
#include <html_dom.h>
#include <html_ui.h>
#include <app_mode.h> // app_route_url_target
#include <spectate.h> // spectate_active
#include <net_ext.h>
#include <klog.h>
#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void show(const char *id, const char *display) {
    const HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, display);
}

// ---------------------------------------------------------------- chreq: KR charge consent

#define CHARGE_TIMEOUT_MS 15000.0 // JfCzGzvGIQB8rrJX(iìîíïìì, 15000) (:257697); the server declines at 15 s too (:221241)

static struct {
    int pending; // iïîiîiï: a request waits for our answer
    int id;
    int modal_up;
    int timer;
    Client *client;
} g_charge;

// iìîíïìì: the request is forgotten, both prompts hidden
static void charge_close(void) {
    const int had_modal = g_charge.modal_up;
    g_charge.pending = 0;
    g_charge.modal_up = 0;
    if (g_charge.timer) html_timer_cancel(g_charge.timer);
    g_charge.timer = 0;
    if (html_ui_available() && html_ui_is_ready()) {
        show("chargeConfirm", "none");
        show("chargeModal", "none");
    }
    if (had_modal && g_charge.client) client_pointer_update(g_charge.client); // the lock may come back
}

static void charge_timeout(void *user) {
    (void) user;
    g_charge.timer = 0;
    charge_close();
}

// iïííìíì: ["chrsp", id, 1] accepts, ["chrsp", id] declines (on our wire NET_EXT_CHARGE_CONSENT_RESPONSE); the prompt
// closes either way. A client with no connection sends nothing (client_net_send drops it).
static void charge_answer(const int accept) {
    if (!g_charge.pending) return;
    const NetChargeConsentResponsePacket r = {.id = g_charge.id, .accept = (uint8_t) !!accept};
    uint8_t buf[NET_EXT_HEADER_SIZE + sizeof(r)];
    const uint16_t n = net_ext_pack(buf, sizeof(buf), NET_EXT_CHARGE_CONSENT_RESPONSE, &r, sizeof(r));
    if (n && g_charge.client) client_net_send(g_charge.client, (NetPacketType) NET_PACKET_EXT, buf, n);
    KN_LOG_VERBOSE2("[charge] request %d %s\n", g_charge.id, accept ? "accepted" : "declined");
    charge_close();
}

// chargeModalAccept.onclick / chargeModalDecline.onclick: knChargeConsent(1) / knChargeConsent(0)
static int h_charge_consent(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    charge_answer(argc > 0 && args[0].kind == HTML_ARG_NUM && args[0].num != 0);
    return 1;
}

void s2c_ui_charge_request(Client *client, const int id, const double amount, const int modal) {
    g_charge.client = client;
    g_charge.pending = 1;
    g_charge.id = id;
    char text[96];
    snprintf(text, sizeof(text), "This game wants to charge you %.0f KR", floor(amount > 0 ? amount : 0)); // Math.floor(amount || 0)
    if (html_ui_available() && html_ui_is_ready()) {
        if (modal) {
            const HtmlElem info = html_dom_by_id("chargeModalInfo"), accept = html_dom_by_id("chargeModalAccept"),
                           decline = html_dom_by_id("chargeModalDecline");
            html_ui_add_handler("knChargeConsent", h_charge_consent);
            if (info) html_dom_set_html(info, text);
            if (accept) html_dom_set_attr(accept, "onclick", "knChargeConsent(1)");
            if (decline) html_dom_set_attr(decline, "onclick", "knChargeConsent(0)");
            show("chargeModal", "flex");
            g_charge.modal_up = 1;
            client_pointer_update(client); // `if (CONTROLS.hasPointerlock) CONTROLS.toggle(false)`
        } else {
            const HtmlElem info = html_dom_by_id("chargeConfirmInfo");
            if (info) html_dom_set_html(info, text);
            show("chargeConfirm", "block");
        }
    }
    if (g_charge.timer) html_timer_cancel(g_charge.timer); // clearTimeout(iìíîíìï)
    g_charge.timer = html_timer_add(CHARGE_TIMEOUT_MS, charge_timeout, NULL);
    KN_LOG_VERBOSE2("[charge] request %d: %s%s\n", id, text, modal ? " (modal)" : "");
}

// The keydown branch (:259942-259951): kickVoteY accepts, kickVoteN declines while a request waits. v10 takes them only
// while no vote kick runs; here a key the vote kick did not take (hud_votekick_key returned 0: none runs, we voted
// already or we are dead) answers the request.
int s2c_ui_charge_key(Client *client, const int yes) {
    (void) client;
    if (!g_charge.pending) return 0;
    charge_answer(yes);
    return 1;
}

int s2c_ui_charge_modal_open(void) { return g_charge.pending && g_charge.modal_up; }

// ---------------------------------------------------------------- warsClan: Turf Wars

#define WARS_POPUP_DELAY_MS 1000.0 // JfCzGzvGIQB8rrJX(..., 1000) (:242102, :255569)

static struct {
    int has;            // territorialWars.clanInfo
    char name[32];      // its clan_name
    int shown;          // game.shownWarsAnnouncement: reset at every init (:256778)
    int entered;        // iíîîìïì: from enterGame until the menu shows again (:242079, :243854)
    int timer;
    char popup_name[32]; // the clan the scheduled popup names
} g_wars;

static void wars_popup(void *user) {
    (void) user;
    g_wars.timer = 0;
    if (g_wars.shown) return;
    g_wars.shown = 1;
    char sub[96];
    snprintf(sub, sizeof(sub), "Territory controlled by %s", g_wars.popup_name);
    hud_popups_text_popup("Turf Wars", sub, 0, 0); // iíìíiìì(title, sub): not silent
}

static void wars_schedule(void) {
    snprintf(g_wars.popup_name, sizeof(g_wars.popup_name), "%s", g_wars.name);
    if (!g_wars.timer) g_wars.timer = html_timer_add(WARS_POPUP_DELAY_MS, wars_popup, NULL);
}

void s2c_ui_wars_clan(Client *client, const int has, const char *clan_name) {
    (void) client;
    g_wars.has = has;
    snprintf(g_wars.name, sizeof(g_wars.name), "%s", has && clan_name ? clan_name : "");
    if (has && g_wars.entered) wars_schedule(); // `if (info && iíîîìïì)`, the popup itself once (:255563-255570)
}

// ---------------------------------------------------------------- init / per frame

void s2c_ui_on_init(Client *client) {
    (void) client;
    charge_close(); // :257109
    g_wars.has = 0; // setClanInfo(null) unless init's syncData has one (the EXT after the INIT)
    g_wars.name[0] = 0;
    g_wars.shown = 0;
    // a popup still scheduled from the game before: v10's timer would fire into the new game, this one is dropped
    if (g_wars.timer) html_timer_cancel(g_wars.timer);
    g_wars.timer = 0;
}

// Entering the game (enterGame's `if (clanInfo && !shown)` schedule, :242095-242104): the menu closes on a ready game
// that we play or spectate. The client's enterGame (client_enter_game) closes the menu the same frame.
void s2c_ui_tick(Client *client) {
    const int entered = client && client->game.ready && html_ui_available() && html_ui_is_ready() && !html_ui_is_menu_open() &&
                        ((client->me && client->me->active) || spectate_active() || client->spawn_requested);
    if (entered && !g_wars.entered && g_wars.has && !g_wars.shown) wars_schedule();
    g_wars.entered = entered;
}

// ---------------------------------------------------------------- edMp: the editor

#define MAP_TO_EDIT_KEY "mapToEdit" // the editor's checkEdit reads it first (editor_io.c ed_map_restore; ui_mod_keys.c)

void s2c_ui_edit_map(const char *json, const size_t len) {
    if (!json || !len) return;
    char *text = NULL;
    cJSON *map = cJSON_ParseWithLength(json, len);
    const cJSON *objects = cJSON_GetObjectItemCaseSensitive(map, "objects");
    if (cJSON_IsArray(objects)) { // JSON.parse(map).objects.forEach: col === false / l === false deleted
        cJSON *o;
        cJSON_ArrayForEach(o, objects) {
            if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "col"))) cJSON_DeleteItemFromObjectCaseSensitive(o, "col");
            if (cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(o, "l"))) cJSON_DeleteItemFromObjectCaseSensitive(o, "l");
        }
        text = cJSON_PrintUnformatted(map);
    }
    cJSON_Delete(map);
    if (!text) { // the try's catch: the map as it came
        text = malloc(len + 1);
        if (!text) return;
        memcpy(text, json, len);
        text[len] = 0;
        html_storage_set(MAP_TO_EDIT_KEY, text);
        free(text);
    } else {
        html_storage_set(MAP_TO_EDIT_KEY, text);
        cJSON_free(text);
    }
    KN_LOG_VERBOSE2("[editor] edMp: a %zu byte map staged, the editor opens\n", len);
    app_route_url_target("editor.html", 1); // window.open("/editor.html", "_blank")
}
