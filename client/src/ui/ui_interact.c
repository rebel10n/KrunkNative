// Krunker v7.1.4 interaction, client side (client/include/ui_interact.h):
//   players.js updateInteract (v7 src/data/players.js:3808-3935): the #interactMsg line of the
//     changeInteract winner this tick (game_interact_scan, shared/src/interact.c): gates (0), deposit
//     boxes (1), onHoldMouse triggers (4), weapon pickups (6), onInteractKey triggers (7, with the
//     "inm" text and the cost tag), and #interactProg / #interProgB for the hold progress.
//   index.js keys (:16494-16523): interactKey -> "int" type, uid; interactSecKey -> "intSec";
//     dropKey -> "drpw"; confirmKey -> "cnfm" while pendingTrigger is set.
// A server older than NET_PROTOCOL_MINOR_V7 gets the v3 "int" 1[, true] (it resolves the target).
#include <ui_interact.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <zombies.h> // map-logic WP-H: machine / airdrop prompts
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ruleset.h>
#include <ui_mobile.h> // the phone prompt: "Tap to ..." and its bottom (v10 updateInteract)

#define INTERACT_KEY_COLOR "#FBC02D"        // COLORS.interactPopup.key
#define INTERACT_ENOUGH_COLOR "#9eeb56"     // COLORS.interactPopup.enough
#define INTERACT_NOT_ENOUGH_COLOR "#eb5656" // COLORS.interactPopup.notEnough
#define INTERACT_BOTTOM "220px"             // updateInteract: interactMsg.style.bottom (desktop)
#define INTERACT_BOTTOM_MOBILE "77px"       // ... on a phone (controls.isMobile: the #mobileUI buttons sit below it)

static struct {
    int prompt_shown;
    char prompt[768];
    char scenario[768]; // ui_interact_scenario: the prompt shown in place of the scan's
} g_interact;

void ui_interact_scenario(const char *html) {
    snprintf(g_interact.scenario, sizeof(g_interact.scenario), "%s", html ? html : "");
}

static int in_game(const Client *client) {
    return client && client->me && client->me->active && client->game.ready && !html_ui_is_menu_open();
}

static int v7_server(const Client *client) {
    return client->offline || client->net_server_minor >= NET_PROTOCOL_MINOR_V7;
}

// UTILS.commaFormatNum: "1234567" -> "1,234,567" (a leading "-" stays in front)
static void comma_format(const int value, char *out, const size_t size) {
    char digits[16];
    snprintf(digits, sizeof(digits), "%d", value);
    const char *d = digits[0] == '-' ? digits + 1 : digits;
    const size_t n = strlen(d);
    size_t o = 0;
    if (d != digits && o + 1 < size) out[o++] = '-';
    for (size_t i = 0; i < n && o + 2 < size; i++) {
        if (i && (n - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

static const Weapon *weapon_at(const Game *game, const int id) {
    const size_t count = game->weapon_count ? game->weapon_count : WEAPON_COUNT;
    const Weapon *const *all = game->weapons ? (const Weapon *const *) game->weapons : rules_weapons();
    return id >= 0 && (size_t) id < count ? all[id] : NULL;
}

// What the client knows of its own account: player.account (has_account). Level, premium,
// verified and partner arrive with the account info; natively they read as not enough.
static int method_enough(const Player *me, const int method, const int price, const int points) {
    switch (method) {
        case 0:
        case 1: return me->score >= price;
        case 2: return (int) me->kills >= points;
        case 3: return me->has_account;
        default: return 0;
    }
}

// " [<price> Kills]" / " [Account Only]" / " [Level N]" of a gate / pickup method (0 none)
static void method_tag(char *out, const size_t size, const int method, const int price, const int enough) {
    out[0] = 0;
    if (!price && method <= 2) return;
    char what[48];
    if (method > 2 && method != 7) {
        snprintf(what, sizeof(what), "%s Only", method == 3 ? "Account" : method == 4 ? "Verified" : method == 5 ? "Premium" : "Partner");
    } else {
        snprintf(what, sizeof(what), "%s%d%s", method == 7 ? "Level " : "", price, method == 2 ? " Kills" : "");
    }
    snprintf(out, size, " <span style='color:%s'>[%s]</span>", enough ? INTERACT_ENOUGH_COLOR : INTERACT_NOT_ENOUGH_COLOR, what);
}

static void key_label(const int bind, char *out, const size_t size) {
    char key[32];
    ui_controls_key_name(ui_controls_bind(bind), key, sizeof(key));
    snprintf(out, size, "<span style=\"color:%s\">[%s]</span>", INTERACT_KEY_COLOR, key);
}

static void build_prompt(const Client *client, char *html, const size_t size) {
    GameInteractScan scan;
    game_interact_scan(&client->game, client->me, &scan);
    const Player *me = client->me;
    const Object *object = scan.index >= 0 && (size_t) scan.index < client->game.map->object_count ? client->game.map->objects[scan.index] : NULL;
    char key[64], tag[160];
    key_label(UI_CONTROL_INTERACT, key, sizeof(key));
    html[0] = 0;
    switch (scan.type) {
        case GAME_INTERACT_GATE: {
            const int method = object->gate_method, price = (int) object->score_points;
            method_tag(tag, sizeof(tag), method, price, method_enough(me, method, price, price));
            // v10 updateInteract (type 0): a phone reads "Tap" (the prompt is the touch, see ui_mobile.c)
            char lead[80];
            if (ui_mobile_active()) snprintf(lead, sizeof(lead), "Tap");
            else snprintf(lead, sizeof(lead), "Press %s", key);
            if (object->interact_msg) snprintf(html, size, "%s to %s%s", lead, object->interact_msg, tag);
            else snprintf(html, size, "%s to %s gate%s", lead, !object->active && object->closeable ? "Close" : "Open", tag);
            break;
        }
        case GAME_INTERACT_BANK: {
            char deposited[32], deposit[32], withdraw[32], sec[64];
            comma_format(object->deposited, deposited, sizeof(deposited));
            if (object->deposit_amount) snprintf(deposit, sizeof(deposit), "%d", object->deposit_amount); else snprintf(deposit, sizeof(deposit), "All");
            if (object->withdraw_amount) snprintf(withdraw, sizeof(withdraw), "%d", object->withdraw_amount); else snprintf(withdraw, sizeof(withdraw), "All");
            key_label(UI_CONTROL_INTERACT_SEC, sec, sizeof(sec));
            snprintf(html, size,
                     "<span style='color:%s'>[%s]</span><div style='color: #fff;margin-top:-30px;'>Press %s to Deposit - %s</div>"
                     "<div style='color: #fff;margin-top:-30px;'>Press %s to Withdraw - %s</div>",
                     INTERACT_NOT_ENOUGH_COLOR, deposited, key, deposit, sec, withdraw);
            break;
        }
        case GAME_INTERACT_TRIGGER: // getKeyName(10001): "Hold [M1] to <inm || Interact>"
            snprintf(html, size, "Hold <span style=\"color:%s\">[M1]</span> to %s", INTERACT_KEY_COLOR,
                     object && object->interact_msg ? object->interact_msg : "Interact");
            break;
        case GAME_INTERACT_PICKUP: {
            GamePickup pickup;
            if (!game_pickup_state(&client->game, scan.index, &pickup)) break;
            const Weapon *w = weapon_at(&client->game, pickup.weapon);
            const char *name = w && w->name ? w->name : "weapon";
            const int unrestricted = client->game.mode && client->game.mode->config.unrestricted_weapons;
            int held = -1;
            for (int i = 0; me->loadout && i < me->loadout_size; i++) {
                if (me->loadout[i] == pickup.weapon) held = i;
            }
            const int upgraded = held >= 0 && held < 8 && (me->upgraded_slots >> held & 1);
            const int price = unrestricted && held >= 0 && pickup.method < 2 ? (upgraded ? 4500 : (pickup.score + 1) / 2) : pickup.score;
            method_tag(tag, sizeof(tag), pickup.method, price, method_enough(me, pickup.method, price, pickup.score));
            if (unrestricted && pickup.method < 2) snprintf(html, size, "Press %s to buy %s%s%s", key, name, held >= 0 ? " ammo" : "", tag);
            else if (object && object->interact_msg) snprintf(html, size, "Press %s to %s%s", key, object->interact_msg, tag);
            else snprintf(html, size, "Press %s to pickup %s%s", key, name, tag);
            break;
        }
        case GAME_INTERACT_KEY: {
            const int pending = me->pending_trigger == scan.index + 1;
            char confirm[64], confirm_key[32];
            ui_controls_key_name(ui_controls_v7_bind("confirm", -1, 0), confirm_key, sizeof(confirm_key)); // binds.confirm (K)
            snprintf(confirm, sizeof(confirm), "<span style=\"color:%s\">[%s]</span>", INTERACT_KEY_COLOR, confirm_key);
            const TriggerData *t = object ? object->trig : NULL;
            const int method = t ? t->action_method : 0, cost = t ? t->action_cost : 0;
            tag[0] = 0;
            if (cost || method > 3) {
                // the KR balance is the account's (window.canAfford): not known natively, so "not enough"
                const int enough = method == 0 ? 0 : method < 3 ? me->score >= cost : method == 3 ? (int) me->kills >= cost : method == 4 ? me->has_account : 0;
                char what[48];
                if (method > 3 && method != 8) snprintf(what, sizeof(what), "%s Only", method == 4 ? "Account" : method == 5 ? "Verified" : method == 6 ? "Premium" : "Partner");
                else snprintf(what, sizeof(what), "%s%d%s", method == 8 ? "Level " : "", cost, method == 0 ? " KR" : method == 3 ? " Kills" : "");
                snprintf(tag, sizeof(tag), " <span style='color:%s'>[%s]</span>", enough ? INTERACT_ENOUGH_COLOR : INTERACT_NOT_ENOUGH_COLOR, what);
            }
            snprintf(html, size, "Press %s to %s%s", pending ? confirm : key,
                     pending ? "Confirm" : object && object->interact_msg ? object->interact_msg : "Interact", tag);
            break;
        }
        case GAME_INTERACT_MYSTERY_BOX: { // map-logic WP-H (players.js updateInteract 13)
            const ZombieMachine *m = zombies_machine(&client->game, scan.index);
            const int price = zombies_mystery_box_price(&client->game, me, object);
            if (m && m->state >= 2 && m->pending_weapon != ZOMBIES_NONE) snprintf(html, size, "Press %s for weapon", key);
            else snprintf(html, size, "Press %s for a Random Weapon <span style='color:%s'>[Cost %d]</span>", key,
                          me->score >= price ? INTERACT_ENOUGH_COLOR : INTERACT_NOT_ENOUGH_COLOR, price);
            break;
        }
        case GAME_INTERACT_PERK: { // 14
            const int price = object->score_points ? (int) object->score_points : 1;
            if (me->perk_count >= GAME_MAX_ZOMBIE_PERKS) snprintf(html, size, "<span style='color:%s'>Perk limit reached</span>", INTERACT_NOT_ENOUGH_COLOR);
            else snprintf(html, size, "Press %s to buy %s <span style='color:%s'>[Cost %d]</span>", key, g_zombie_perk_names[object->zombie_perk % ZOMBIES_PERK_COUNT],
                          me->score >= price ? INTERACT_ENOUGH_COLOR : INTERACT_NOT_ENOUGH_COLOR, price);
            break;
        }
        case GAME_INTERACT_KRUM: { // 15
            const ZombieMachine *m = zombies_machine(&client->game, scan.index);
            const int price = object->score_points ? (int) object->score_points : 1;
            if (m && m->state >= 2 && m->pending_weapon != ZOMBIES_NONE) snprintf(html, size, "Press %s to take your upgraded weapon", key);
            else snprintf(html, size, "Press %s to Krum It <span style='color:%s'>[Cost %d]</span>", key,
                          me->score >= price ? INTERACT_ENOUGH_COLOR : INTERACT_NOT_ENOUGH_COLOR, price);
            break;
        }
        case GAME_INTERACT_CLAIM: { // 10: "Hold [key] to Claim <name>" in the rarity colour (index = drop slot)
            if (scan.index < 0 || scan.index >= AIRDROP_MAX) break;
            const AirDropPackage *package = &g_airdrop_packages[client->game.zombies.drops[scan.index].index];
            snprintf(html, size, "Hold %s to Claim <span style='color:%s'>%s</span>", key, g_airdrop_rarity_colors[package->rarity], package->name);
            break;
        }
        default:
            break;
    }
}

static void update_prompt(Client *client) {
    const HtmlElem msg = html_dom_by_id("interactMsg");
    if (!msg) return;
    char html[sizeof(g_interact.prompt)] = "";
    if (in_game(client)) {
        if (g_interact.scenario[0]) snprintf(html, sizeof(html), "%s", g_interact.scenario);
        else build_prompt(client, html, sizeof(html));
    }
    if (html[0]) {
        if (!g_interact.prompt_shown || strcmp(html, g_interact.prompt)) {
            html_dom_set_html(msg, html);
            snprintf(g_interact.prompt, sizeof(g_interact.prompt), "%s", html);
        }
        // v7 updateInteract writes the display every frame: the HUD's passes (toggleGameUI,
        // the visibility pass, the spectator HUD) hide #interactMsg without telling us.
        char display[16];
        if (html_dom_get_style(msg, "display", display, sizeof(display)) < 0 || strcmp(display, "block") != 0) html_dom_set_display(msg, "block");
        // ... and its bottom (players.js:3940): main.css gives #interactMsg no top/bottom, so
        // without it the box sits at its static position, the top of the zero-height
        // #midGameUIHider (the top edge of the screen). 220px on desktop (77px is mobile,
        // "50%" the Opensea prompt, type 11: neither is ported).
        const char *bottom_px = ui_mobile_active() ? INTERACT_BOTTOM_MOBILE : INTERACT_BOTTOM;
        char bottom[16];
        if (html_dom_get_style(msg, "bottom", bottom, sizeof(bottom)) < 0 || strcmp(bottom, bottom_px) != 0) html_dom_set_style(msg, "bottom", bottom_px);
        g_interact.prompt_shown = 1;
    } else if (g_interact.prompt_shown) {
        html_dom_set_html(msg, "");
        html_dom_set_display(msg, "none");
        g_interact.prompt_shown = 0;
    }
}

static double setting_or(const char *key, const double def) {
    double v;
    return ui_setting_number(key, &v) && v == v ? v : def;
}

// The bar as the Editing tab's prog* settings place it (v10.js:206825..206900; drawn on the overlay
// canvas at v10.js:160627, the DOM bar here): a 400 x 30 px (x progScale, `|| 1`) roundRect at
// left = progOffX * 10 %, top = progOffY * 10 % of the overlay (its top centre; defaults 50 % / 72 %),
// rotated progRot degrees there, at globalAlpha progOpac; the track rgba(0,0,0,.4) radius 4, the fill
// progColor radius 2, both stroked #000000 (lineWidth 1) with progShadow.
static void apply_progress_style(const HtmlElem holder, const HtmlElem bar) {
    static char applied[256];
    const cJSON *col = ui_setting_value("progColor");
    const char *color = cJSON_IsString(col) && cJSON_GetStringValue(col)[0] ? cJSON_GetStringValue(col) : "#ffffff";
    double scale = setting_or("progScale", 1.0);
    if (scale == 0.0) scale = 1.0;
    const double left = setting_or("progOffX", 5.0) * 10.0, top = setting_or("progOffY", 7.2) * 10.0;
    const double rot = setting_or("progRot", 0.0) * M_PI / 180.0, opacity = setting_or("progOpac", 1.0);
    const int shadow = !ui_setting_is_false("progShadow");
    char sig[256];
    snprintf(sig, sizeof(sig), "%s|%g|%g|%g|%g|%g|%d", color, scale, left, top, rot, opacity, shadow);
    if (!strcmp(sig, applied)) return;
    snprintf(applied, sizeof(applied), "%s", sig);
    char v[96];
    snprintf(v, sizeof(v), "%g%%", left);
    html_dom_set_style(holder, "left", v);
    snprintf(v, sizeof(v), "%g%%", top);
    html_dom_set_style(holder, "top", v);
    html_dom_set_style(holder, "bottom", "auto");
    snprintf(v, sizeof(v), "%gpx", 400.0 * scale);
    html_dom_set_style(holder, "width", v);
    snprintf(v, sizeof(v), "%gpx", 30.0 * scale);
    html_dom_set_style(holder, "height", v);
    html_dom_set_style(holder, "transform-origin", "50% 0");
    snprintf(v, sizeof(v), "translate(-50%%, 0) rotate(%grad)", rot);
    html_dom_set_style(holder, "transform", v);
    snprintf(v, sizeof(v), "%g", opacity);
    html_dom_set_style(holder, "opacity", v);
    html_dom_set_style(holder, "background-color", "rgba(0, 0, 0, 0.4)");
    html_dom_set_style(holder, "border-radius", "4px");
    html_dom_set_style(holder, "box-shadow", shadow ? "0 0 0 1px #000000" : "none");
    if (bar) {
        html_dom_set_style(bar, "background-color", color);
        html_dom_set_style(bar, "border-radius", "2px");
        html_dom_set_style(bar, "box-shadow", shadow ? "0 0 0 1px #000000" : "none");
    }
}

// updateInteractProgress ("p" = the player's interProg): #interactProg shows while > 0,
// #interProgB is the bar. The local copy computes the same interact_progress.
static void update_progress(Client *client) {
    static float shown = -1.0f;
    const float progress = in_game(client) ? roundf(client->me->interact_progress * 1000.0f) / 1000.0f : 0.0f;
    const HtmlElem holder = html_dom_by_id("interactProg");
    const HtmlElem bar = html_dom_by_id("interProgB");
    if (holder && progress > 0.0f) apply_progress_style(holder, bar);
    if (progress == shown) return;
    if (holder) html_dom_set_display(holder, progress > 0.0f ? "block" : "none");
    if (bar && progress > 0.0f) {
        char width[32];
        snprintf(width, sizeof(width), "%g%%", progress * 100.0f);
        html_dom_set_style(bar, "width", width);
    }
    shown = progress;
}

void ui_interact_tick(Client *client) {
    if (!html_ui_available() || !html_ui_is_ready()) return;
    update_prompt(client);
    update_progress(client);
}

void ui_interact_key(Client *client, const int secondary) {
    if (!client || !client->me || !client->me->active) return;
    if (!v7_server(client)) {
        const NetInteractPacket packet = {.action = 1, .secondary = (uint8_t) !!secondary};
        client_net_send(client, NET_PACKET_INTERACT, &packet, sizeof(packet)); // v3 io.send("int", 1[, true])
        return;
    }
    GameInteractScan scan;
    game_interact_scan(&client->game, client->me, &scan);
    const NetInteractV7Packet packet = {.action = 1, .secondary = (uint8_t) !!secondary, .type = scan.last_type, .uid = scan.last_uid};
    client_net_send(client, NET_PACKET_INTERACT, &packet, sizeof(packet)); // io.send("int" / "intSec", type, id)
}

void ui_interact_drop(Client *client) {
    if (!client || !client->me || !client->me->active) return;
    if (v7_server(client)) {
        const NetInteractV7Packet packet = {.action = 2, .type = GAME_INTERACT_NONE, .uid = -1};
        client_net_send(client, NET_PACKET_INTERACT, &packet, sizeof(packet)); // io.send("drpw")
    } else {
        const NetInteractPacket packet = {.action = 2};
        client_net_send(client, NET_PACKET_INTERACT, &packet, sizeof(packet));
    }
}

int ui_interact_confirm(Client *client) {
    if (!client || !client->me || !client->me->pending_trigger || !v7_server(client) || html_ui_is_menu_open()) return 0;
    const NetConfirmPacket packet = {.uid = client->me->pending_trigger - 1, .kind = 0};
    client_net_send(client, NET_PACKET_CONFIRM_INTERACT, &packet, sizeof(packet)); // io.send("cnfm")
    client->me->pending_trigger = 0;
    return 1;
}
